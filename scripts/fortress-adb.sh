#!/bin/sh
# Установка брони: deactivate из облачного конфига блокируется.
# Запускать на колонке от root. Оригинал сохраняется рядом (.real).
Q=/system/vendor/quasar
BK=/data/local/tmp/backup_q
mkdir -p "$BK"

if [ ! -f "$Q/activate_adb.sh.real" ]; then
    cp -f "$Q/activate_adb.sh" "$BK/activate_adb.sh.orig"
    cp -f "$Q/activate_adb.sh" "$Q/activate_adb.sh.real"
    chmod 755 "$Q/activate_adb.sh.real"
fi

cat > "$Q/activate_adb.sh" <<'EOS'
#!/bin/sh
for a in "$@"; do
    case "$a" in
        deactivate)
            echo "$(date): fortress blocked deactivate" >> /data/quasar/quasar_adb.log
            exit 0
            ;;
    esac
done
exec /system/vendor/quasar/activate_adb.sh.real "$@"
EOS
chmod 755 "$Q/activate_adb.sh"
sync
echo "fortress installed"
