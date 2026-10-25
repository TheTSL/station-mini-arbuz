import json
import os
import time
import traceback

import requests

BASE_DIR = "/opt/tg-bot"
PANEL_DIR = "/opt/station-panel"
TOKEN = open(os.path.join(BASE_DIR, "token")).read().strip()
API = "https://api.telegram.org/bot" + TOKEN

STATE = os.path.join(PANEL_DIR, "state.json")
MODE = os.path.join(PANEL_DIR, "desired_mode")
VOL = os.path.join(PANEL_DIR, "desired_volume")
MUTE = os.path.join(PANEL_DIR, "desired_mute")
ALLOWED = os.path.join(BASE_DIR, "allowed_chat")


def read(path, default=""):
    try:
        return open(path).read().strip()
    except Exception:
        return default


def write(path, value):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(value))
    os.replace(tmp, path)


def allowed_chat():
    return read(ALLOWED)


def set_allowed(chat_id):
    write(ALLOWED, chat_id)


def state():
    try:
        return json.load(open(STATE))
    except Exception:
        return {}


def fmt_status():
    d = state()
    if not d:
        return "Нет данных с колонки."
    age = int(time.time()) - d.get("_ts", 0) if d.get("_ts") else None

    def yn(v):
        return "да" if v else "нет"

    lines = [
        f"*{d.get('model', '—')}*",
        f"Прошивка: `{d.get('version', '—')}`",
        f"Режим: *{d.get('mode', '—')}*" + (f" (запрошено: {d.get('desired')})" if d.get("desired") else ""),
        f"Uptime: {d.get('uptime', '—')}",
        f"RAM свободно: {d.get('mem_free', '—')}",
        f"Load: {d.get('load', '—')}",
        "",
        f"maind platformd: {yn(d.get('maind_platformd'))}",
        f"maind mediad: {yn(d.get('maind_mediad'))}",
        f"ADB: {yn(d.get('adb'))} · SSH: {yn(d.get('ssh'))}",
        "",
        f"Громкость: *{d.get('volume', '—')}* · mute: {d.get('mute', '—')}",
        f"Wi-Fi: {d.get('wifi_ssid', '—')} ({d.get('wifi_signal', '—')})",
        f"Температура: {d.get('temp', '—')}",
        f"Датчик: {d.get('distance', '—')}",
    ]
    if age is not None:
        lines.append(f"\nОбновлено {age} с назад")
    return "\n".join(lines)


KEYBOARD = {
    "inline_keyboard": [
        [{"text": "Алиса", "callback_data": "mode:alice"},
         {"text": "Свой режим", "callback_data": "mode:custom"}],
        [{"text": "Громкость −10", "callback_data": "vol:-10"},
         {"text": "Громкость +10", "callback_data": "vol:+10"}],
        [{"text": "Mute", "callback_data": "mute:on"},
         {"text": "Unmute", "callback_data": "mute:off"}],
        [{"text": "Статус", "callback_data": "status"}],
    ]
}


def send(chat_id, text, keyboard=True):
    payload = {"chat_id": chat_id, "text": text, "parse_mode": "Markdown"}
    if keyboard:
        payload["reply_markup"] = json.dumps(KEYBOARD)
    try:
        requests.post(API + "/sendMessage", data=payload, timeout=20)
    except Exception:
        pass


def answer_cb(cb_id, text=""):
    try:
        requests.post(API + "/answerCallbackQuery",
                      data={"callback_query_id": cb_id, "text": text}, timeout=15)
    except Exception:
        pass


def handle(chat_id, text):
    text = (text or "").strip()
    low = text.lower()

    if low in ("/start", "/help", "start"):
        send(chat_id, "Бот колонки. /status — состояние, /alice и /custom — режим, /vol 40 — громкость, /mute и /unmute.")
        send(chat_id, fmt_status())
        return

    if low in ("/status", "статус"):
        send(chat_id, fmt_status())
        return

    if low in ("/alice", "алиса"):
        write(MODE, "alice")
        send(chat_id, "Запрошен режим Алисы. Колонка перезагрузится в течение ~15 с.")
        return

    if low in ("/custom", "свой"):
        write(MODE, "custom")
        send(chat_id, "Запрошен свой режим. Колонка перезагрузится в течение ~15 с.")
        return

    if low.startswith("/vol"):
        parts = low.split()
        if len(parts) == 2 and parts[1].lstrip("+-").isdigit():
            v = max(0, min(100, int(parts[1])))
            write(VOL, v)
            send(chat_id, f"Громкость → {v}%.")
        else:
            send(chat_id, "Использование: /vol 40")
        return

    if low == "/mute":
        write(MUTE, "on")
        send(chat_id, "Mute включён.")
        return

    if low == "/unmute":
        write(MUTE, "off")
        send(chat_id, "Mute выключен.")
        return

    send(chat_id, "Не понял. /status, /alice, /custom, /vol 40, /mute, /unmute")


def main():
    offset = 0
    while True:
        try:
            r = requests.get(API + "/getUpdates",
                             params={"offset": offset, "timeout": 30}, timeout=40)
            data = r.json()
            for upd in data.get("result", []):
                offset = upd["update_id"] + 1

                if "callback_query" in upd:
                    cq = upd["callback_query"]
                    chat_id = str(cq["message"]["chat"]["id"])
                    if allowed_chat() and chat_id != allowed_chat():
                        answer_cb(cq["id"], "Нет доступа")
                        continue
                    data_cb = cq.get("data", "")
                    if data_cb == "status":
                        answer_cb(cq["id"], "Статус")
                        send(chat_id, fmt_status())
                    elif data_cb.startswith("mode:"):
                        m = data_cb.split(":", 1)[1]
                        write(MODE, m)
                        answer_cb(cq["id"], "Режим: " + m)
                        send(chat_id, f"Запрошен режим *{m}*. Колонка перезагрузится.")
                    elif data_cb.startswith("vol:"):
                        delta = int(data_cb.split(":", 1)[1])
                        cur = state().get("volume", "0%").replace("%", "")
                        try:
                            base = int(cur)
                        except Exception:
                            base = 50
                        v = max(0, min(100, base + delta))
                        write(VOL, v)
                        answer_cb(cq["id"], f"{v}%")
                        send(chat_id, f"Громкость → {v}%.")
                    elif data_cb.startswith("mute:"):
                        m = data_cb.split(":", 1)[1]
                        write(MUTE, m)
                        answer_cb(cq["id"], m)
                        send(chat_id, "Mute " + m)
                    continue

                msg = upd.get("message") or upd.get("edited_message")
                if not msg:
                    continue
                chat_id = str(msg["chat"]["id"])
                if not allowed_chat():
                    set_allowed(chat_id)
                    send(chat_id, "Доступ привязан к этому чату. Отправь /status.")
                    continue
                if chat_id != allowed_chat():
                    continue
                handle(chat_id, msg.get("text", ""))
        except Exception:
            traceback.print_exc()
            time.sleep(3)


if __name__ == "__main__":
    main()
