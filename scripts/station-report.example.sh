#!/bin/sh
# Collect station status, post it to the VPS API, apply remote commands.
PANEL="https://PANEL_HOST:PORT/station"
TOKEN="CHANGE_ME_TOKEN"
OUT=/tmp/station-report.json
Q=/system/vendor/quasar
ALICE_TPL=/data/local/tmp/backup_q/launch.alice.json
LAST_SWITCH=/data/local/tmp/last_switch

amx() {
    LD_LIBRARY_PATH=/system/vendor/quasar/libs /system/workdir/bin/amixer "$@" 2>/dev/null
}
b() { [ "$1" = "1" ] && echo true || echo false; }

VER=$(cat $Q/version_full 2>/dev/null | tr -d '\n')
UP=$(cut -d. -f1 /proc/uptime)
UP_H=$((UP / 3600)); UP_M=$(((UP % 3600) / 60))
UPTIME="${UP_H}h ${UP_M}m"

MEMFREE=$(( $(awk '/MemFree/{print $2}' /proc/meminfo) / 1024 ))
LOAD=$(cut -d' ' -f1-3 /proc/loadavg)

if ps | grep -q '[m]aind --platformd'; then PD=1; else PD=0; fi
if ps | grep -q '[m]aind --mediad'; then MD=1; else MD=0; fi
if ps | grep -q '[v]olgest'; then VG=1; else VG=0; fi
if [ "$PD" = "1" ] || [ "$MD" = "1" ]; then MODE="normal"; else MODE="maind-free"; fi
# РґР»СЏ РїРµСЂРµРєР»СЋС‡РµРЅРёСЏ СЂРµР¶РёРј РѕРїСЂРµРґРµР»СЏРµРј РїРѕ РєРѕРЅС„РёРіСѓ, Р° РЅРµ РїРѕ РїСЂРѕС†РµСЃСЃР°Рј:
# СЃСЂР°Р·Сѓ РїРѕСЃР»Рµ СЂРµР±СѓС‚Р° maind РµС‰С‘ РЅРµ СЃС‚Р°СЂС‚РѕРІР°Р», Рё СЌС‚Рѕ РІС‹Р·С‹РІР°Р»Рѕ Р»РѕР¶РЅС‹Рµ СЃСЂР°Р±Р°С‚С‹РІР°РЅРёСЏ
if grep -q 'quasar/maind' $Q/launch.json 2>/dev/null; then CFG_MODE="normal"; else CFG_MODE="maind-free"; fi

if netstat -ln 2>/dev/null | grep -q ':5555'; then ADB=1; else ADB=0; fi
if netstat -ln 2>/dev/null | grep -q ':2222'; then SSH=1; else SSH=0; fi

# РњРёРєС€РµСЂ РѕРїСЂР°С€РёРІР°РµРј СЂРµРґРєРѕ (СЂР°Р· РІ 30 СЃ): С‡Р°СЃС‚РѕРµ РѕС‚РєСЂС‹С‚РёРµ ALSA-РєРѕРЅС‚СЂРѕР»РѕРІ
# РЅР° СЌС‚РѕР№ РїСЂРѕС€РёРІРєРµ РґР°С‘С‚ С‰РµР»С‡РєРё РІ Р·РІСѓРєРµ. Р Р°Р±РѕС‚Р°РµС‚ РІ РѕР±РѕРёС… СЂРµР¶РёРјР°С….
AMX_TS=/tmp/amixer_ts
CUR_TS=$(date +%s)
LAST_TS=$(cat $AMX_TS 2>/dev/null || echo 0)
if [ "$CFG_MODE" = "custom" ] && [ $((CUR_TS - LAST_TS)) -gt 30 ] && [ "$UP" -gt 90 ]; then
    VOL=$(amx -c 0 sget Master | grep -o '\[[0-9]*%\]' | head -1 | tr -d '[]')
    MUTE=$(amx -c 0 sget 'SPK mute' | grep -oE '\[(on|off)\]' | head -1 | tr -d '[]')
    echo "$VOL" > /tmp/vol.cache
    echo "$MUTE" > /tmp/mute.cache
    echo "$CUR_TS" > $AMX_TS
else
    VOL=$(cat /tmp/vol.cache 2>/dev/null)
    MUTE=$(cat /tmp/mute.cache 2>/dev/null)
fi

# РџРѕСЃР»Рµ РїРµСЂРµРєР»СЋС‡РµРЅРёСЏ СЂРµР¶РёРјР° РІРѕР·РІСЂР°С‰Р°РµРј РіСЂРѕРјРєРѕСЃС‚СЊ, РєРѕС‚РѕСЂР°СЏ Р±С‹Р»Р° РґРѕ СЂРµР±СѓС‚Р°.
LV=/data/local/tmp/last_volume
if [ -f "$LV" ]; then
    set -- $(cat "$LV")
    LV_TS=$1; LV_VAL=$2
    if [ -n "$LV_TS" ] && [ -n "$LV_VAL" ] && [ $((CUR_TS - LV_TS)) -lt 180 ]; then
        amx -c 0 sset 'Master' "${LV_VAL}%" >/dev/null 2>&1
        VOL=$LV_VAL
        echo "$VOL" > /tmp/vol.cache
    else
        rm -f "$LV"
    fi
fi

SSID=$(iw dev wlan0 link 2>/dev/null | awk '/SSID/{print $2; exit}')
SIG=$(iw dev wlan0 link 2>/dev/null | awk '/signal/{print $2" "$3; exit}')

TEMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
if [ -n "$TEMP" ]; then TEMP=$((TEMP / 1000)); else TEMP=""; fi

DIST=$(cat /tmp/volgest.dist 2>/dev/null)

cat > "$OUT" <<EOF
{"model":"YNDX-0004 / yandexmini","version":"$VER","mode":"$MODE","uptime":"$UPTIME","mem_free":"${MEMFREE} MB","load":"$LOAD","maind_platformd":$(b $PD),"maind_mediad":$(b $MD),"adb":$(b $ADB),"ssh":$(b $SSH),"volume":"$VOL","mute":"$MUTE","wifi_ssid":"$SSID","wifi_signal":"$SIG","temp":"${TEMP}C","volgest":$(b $VG),"distance":"$DIST mm"}
EOF

wget -q -O /dev/null --no-check-certificate \
    --header="Content-Type: application/json" \
    --post-file="$OUT" "$PANEL/api/report?t=$TOKEN" 2>/dev/null

# --- РєРѕРјР°РЅРґС‹ СЃ Р±РѕС‚Р°/РїР°РЅРµР»Рё ---
CTRL=$(wget -q -O - --no-check-certificate "$PANEL/api/control?t=$TOKEN" 2>/dev/null)
TARGET=$(echo "$CTRL" | sed -n 's/.*"desired":"\([a-z]*\)".*/\1/p')
VOL_T=$(echo "$CTRL" | sed -n 's/.*"volume":"\([0-9]*\)".*/\1/p')
MUTE_T=$(echo "$CTRL" | sed -n 's/.*"mute":"\([a-z]*\)".*/\1/p')

if [ -n "$VOL_T" ] && [ "$CFG_MODE" = "custom" ]; then
    CUR_VOL=$(echo "$VOL" | tr -d '%')
    if [ "$VOL_T" != "$CUR_VOL" ]; then
        amx -c 0 sset 'Master' "${VOL_T}%" >/dev/null 2>&1
    fi
fi
if [ -n "$MUTE_T" ] && [ "$MUTE_T" != "$MUTE" ]; then
    amx -c 0 sset 'SPK mute' "$MUTE_T" >/dev/null 2>&1
fi

if [ -n "$TARGET" ] && [ "$TARGET" != "$CFG_MODE" ]; then
    NOW=$(date +%s)
    LAST=0
    [ -f "$LAST_SWITCH" ] && LAST=$(cat "$LAST_SWITCH" 2>/dev/null)
    if [ $((NOW - LAST)) -gt 240 ]; then
        echo "$NOW" > "$LAST_SWITCH"
        CUR_VOL=$(amx -c 0 sget Master | grep -o '\[[0-9]*%\]' | head -1 | tr -d '[]%')
        [ -n "$CUR_VOL" ] && echo "$NOW $CUR_VOL" > /data/local/tmp/last_volume
        if [ "$TARGET" = "alice" ] && [ -f "$ALICE_TPL" ]; then
            cp -f "$ALICE_TPL" $Q/launch.json
            sync
            (sleep 5; reboot) >/dev/null 2>&1 &
        elif [ "$TARGET" = "custom" ]; then
            printf '{\n    "nodes": [\n        { "name": "main", "root": ".", "tasks": [] }\n    ]\n}\n' > $Q/launch.json
            sync
            (sleep 5; reboot) >/dev/null 2>&1 &
        fi
    fi
fi
