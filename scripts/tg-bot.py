import json
import os
import queue
import threading
import time
import traceback

import requests

BASE_DIR = "/opt/tg-bot"
PANEL_DIR = "/opt/station-panel"
TOKEN = open(os.path.join(BASE_DIR, "token")).read().strip()
API = "https://api.telegram.org/bot" + TOKEN
STATION_API = "http://127.0.0.1:18787/api/v1"
STATION_TOKEN = open(os.path.join(BASE_DIR, "station_token")).read().strip()
STATION_HEADERS = {"X-Auth-Token": STATION_TOKEN}
HTTP = requests.Session()
VOLUME_QUEUE = queue.Queue()

STATE = os.path.join(PANEL_DIR, "state.json")
MODE = os.path.join(PANEL_DIR, "desired_mode")
VOL = os.path.join(PANEL_DIR, "desired_volume")
MUTE = os.path.join(PANEL_DIR, "desired_mute")
ALLOWED = os.path.join(BASE_DIR, "allowed_chat")

LIVE = {}          # chat_id -> {"message_id": int, "until": ts}
LIVE_LOCK = threading.Lock()
LIVE_INTERVAL = 3


def read(path, default=""):
    try:
        return open(path).read().strip()
    except Exception:
        return default


def write(path, value):
    if path == MODE:
        endpoint, payload = "mode", {"mode": str(value)}
    elif path == VOL:
        endpoint, payload = "volume", {"value": int(value)}
    elif path == MUTE:
        endpoint, payload = "mute", {"value": str(value)}
    else:
        endpoint = None
    if endpoint:
        try:
            r = HTTP.post(STATION_API + "/" + endpoint,
                          headers=STATION_HEADERS, json=payload, timeout=(2, 4))
            r.raise_for_status()
            return True
        except requests.RequestException:
            return False
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(value))
    os.replace(tmp, path)
    return True


def step_volume(delta):
    try:
        r = HTTP.post(STATION_API + "/volume/step", headers=STATION_HEADERS,
                      json={"delta": int(delta)}, timeout=(2, 4))
        r.raise_for_status()
        return r.json()["volume"]
    except (requests.RequestException, KeyError, ValueError):
        return None


def apply_volume_step(chat_id, message_id, delta, markup):
    v = step_volume(delta)
    if v is None:
        send(chat_id, "⚠️ Не удалось изменить громкость. Проверь режим и связь с колонкой.")
        return
    if markup:
        for row in markup.get("inline_keyboard", []):
            for button in row:
                if button.get("callback_data") == "status":
                    button["text"] = f"🔊 {v}%"
        tg("editMessageReplyMarkup", chat_id=chat_id,
           message_id=message_id, reply_markup=json.dumps(markup))


def volume_loop():
    while True:
        chat_id, message_id, delta, markup = VOLUME_QUEUE.get()
        try:
            apply_volume_step(chat_id, message_id, delta, markup)
        except Exception:
            traceback.print_exc()
        finally:
            VOLUME_QUEUE.task_done()


def state():
    try:
        r = HTTP.get(STATION_API + "/status", headers=STATION_HEADERS, timeout=(2, 4))
        r.raise_for_status()
        return r.json()
    except Exception:
        try:
            return json.load(open(STATE))
        except Exception:
            return {}


def fmt_status(d=None):
    d = d or state()
    if not d:
        return "⚠️ Нет данных с колонки."
    age = int(time.time()) - d.get("_ts", 0) if d.get("_ts") else None
    mode = d.get("mode", "—")
    mode_ru = {"normal": "Алиса", "maind-free": "свой режим"}.get(mode, mode)
    dot = "🟢" if age is not None and age < 15 else "🟡" if age is not None and age < 60 else "🔴"
    mute = d.get("mute", "—")

    def yn(v):
        return "да" if v else "нет"

    lines = [
        f"{dot} *YNDX-0004* · `{d.get('version', '—')}`",
        f"Режим: *{mode_ru}*" + (f" → {d.get('desired')}" if d.get("desired") and d.get("desired") != mode else ""),
        f"Громкость: *{d.get('volume', '—')}* · mute: *{mute}*",
        f"Uptime: {d.get('uptime', '—')} · RAM: {d.get('mem_free', '—')} · load: {d.get('load', '—')}",
        f"Wi-Fi: {d.get('wifi_ssid', '—')} ({d.get('wifi_signal', '—')}) · 🌡 {d.get('temp', '—')}",
        f"ADB: {yn(d.get('adb'))} · SSH: {yn(d.get('ssh'))} · датчик: {d.get('distance', '—')}",
        f"maind: platformd {yn(d.get('maind_platformd'))}, mediad {yn(d.get('maind_mediad'))}",
    ]
    if age is not None:
        lines.append(f"_{age} с назад_")
    return "\n".join(lines)


def keyboard(d=None):
    d = d or state()
    mode = d.get("mode", "")
    vol = d.get("volume", "—")
    mute = d.get("mute", "")
    alice_mark = "✓ " if mode == "normal" else ""
    custom_mark = "✓ " if mode == "maind-free" else ""
    mute_btn = "🔊 Unmute" if mute == "on" else "🔇 Mute"
    return {
        "inline_keyboard": [
            [{"text": f"{alice_mark}Алиса", "callback_data": "mode:alice"},
             {"text": f"{custom_mark}Свой режим", "callback_data": "mode:custom"}],
            [{"text": "−10", "callback_data": "vol:-10"},
             {"text": f"🔊 {vol}", "callback_data": "status"},
             {"text": "+10", "callback_data": "vol:+10"}],
            [{"text": mute_btn, "callback_data": "mute:on" if mute != "on" else "mute:off"}],
            [{"text": "🔄 Обновить", "callback_data": "status"},
             {"text": "⏱ Живой режим", "callback_data": "live"},
             {"text": "⏹ Стоп", "callback_data": "stop"}],
        ]
    }


def tg(method, **kwargs):
    try:
        return HTTP.post(API + "/" + method, data=kwargs, timeout=(3, 8)).json()
    except Exception:
        return {}


def send(chat_id, text, kb=None):
    r = tg("sendMessage", chat_id=chat_id, text=text, parse_mode="Markdown",
           reply_markup=json.dumps(kb or keyboard()))
    res = r.get("result") or {}
    return res.get("message_id")


def edit(chat_id, message_id, text, kb=None):
    tg("editMessageText", chat_id=chat_id, message_id=message_id, text=text,
       parse_mode="Markdown", reply_markup=json.dumps(kb or keyboard()))


def answer_cb(cb_id, text=""):
    tg("answerCallbackQuery", callback_query_id=cb_id, text=text)


def live_loop():
    while True:
        time.sleep(LIVE_INTERVAL)
        now = time.time()
        with LIVE_LOCK:
            items = list(LIVE.items())
        for chat_id, info in items:
            if now > info["until"]:
                with LIVE_LOCK:
                    LIVE.pop(chat_id, None)
                continue
            d = state()
            edit(chat_id, info["message_id"], fmt_status(d), keyboard(d))


def start_live(chat_id, message_id, minutes=30):
    with LIVE_LOCK:
        LIVE[chat_id] = {"message_id": message_id, "until": time.time() + minutes * 60}


def stop_live(chat_id):
    with LIVE_LOCK:
        LIVE.pop(chat_id, None)


def allowed_chat():
    return read(ALLOWED)


def handle(chat_id, text):
    low = (text or "").strip().lower()
    d = state()

    if low in ("/start", "/help", "start"):
        mid = send(chat_id, fmt_status(d))
        start_live(chat_id, mid)
        return

    if low in ("/status", "статус"):
        mid = send(chat_id, fmt_status(d))
        start_live(chat_id, mid)
        return

    if low in ("/live", "живой"):
        mid = send(chat_id, fmt_status(d))
        start_live(chat_id, mid)
        return

    if low in ("/stop", "стоп"):
        stop_live(chat_id)
        send(chat_id, "Обновление остановлено.")
        return

    if low in ("/alice", "алиса"):
        ok = write(MODE, "alice")
        send(chat_id, "Запрошен режим Алисы. Колонка перезагрузится." if ok else "⚠️ Колонка недоступна.")
        return

    if low in ("/custom", "свой"):
        ok = write(MODE, "custom")
        send(chat_id, "Запрошен свой режим. Колонка перезагрузится." if ok else "⚠️ Колонка недоступна.")
        return

    if low.startswith("/vol"):
        if state().get("mode") == "normal":
            send(chat_id, "В режиме Алисы громкость меняется голосом или жестом. Переключись в свой режим, чтобы управлять из бота.")
            return
        parts = low.split()
        if len(parts) == 2 and parts[1].lstrip("+-").isdigit():
            v = max(0, min(100, int(parts[1])))
            ok = write(VOL, v)
            send(chat_id, f"Громкость → {v}%." if ok else "⚠️ Колонка недоступна.")
        else:
            send(chat_id, "Использование: /vol 40")
        return

    if low == "/mute":
        ok = write(MUTE, "on")
        send(chat_id, "Mute включён." if ok else "⚠️ Колонка недоступна.")
        return

    if low == "/unmute":
        ok = write(MUTE, "off")
        send(chat_id, "Mute выключен." if ok else "⚠️ Колонка недоступна.")
        return

    send(chat_id, "Команды: /status (живой режим), /alice, /custom, /vol 40, /mute, /unmute, /stop")


def main():
    threading.Thread(target=live_loop, daemon=True).start()
    threading.Thread(target=volume_loop, daemon=True).start()
    offset = 0
    while True:
        try:
            r = HTTP.get(API + "/getUpdates",
                         params={"offset": offset, "timeout": 25}, timeout=(5, 35))
            data = r.json()
            for upd in data.get("result", []):
                offset = upd["update_id"] + 1

                if "callback_query" in upd:
                    cq = upd["callback_query"]
                    chat_id = str(cq["message"]["chat"]["id"])
                    msg_id = cq["message"]["message_id"]
                    if allowed_chat() and chat_id != allowed_chat():
                        answer_cb(cq["id"], "Нет доступа")
                        continue
                    cb = cq.get("data", "")
                    if cb == "status":
                        answer_cb(cq["id"], "Обновлено")
                        edit(chat_id, msg_id, fmt_status(), keyboard())
                        start_live(chat_id, msg_id)
                    elif cb == "live":
                        answer_cb(cq["id"], "Живой режим включён")
                        start_live(chat_id, msg_id)
                    elif cb == "stop":
                        stop_live(chat_id)
                        answer_cb(cq["id"], "Остановлено")
                        edit(chat_id, msg_id, "⏹ Обновление остановлено.\n\n" + fmt_status(), keyboard())
                    elif cb.startswith("mode:"):
                        m = cb.split(":", 1)[1]
                        ok = write(MODE, m)
                        answer_cb(cq["id"], "Режим: " + m if ok else "Колонка недоступна")
                        edit(chat_id, msg_id, fmt_status(), keyboard())
                    elif cb.startswith("vol:"):
                        delta = int(cb.split(":", 1)[1])
                        markup = cq["message"].get("reply_markup")
                        if markup:
                            markup = json.loads(json.dumps(markup))
                        VOLUME_QUEUE.put((chat_id, msg_id, delta, markup))
                        answer_cb(cq["id"], "Команда отправляется на колонку…")
                    elif cb.startswith("mute:"):
                        m = cb.split(":", 1)[1]
                        ok = write(MUTE, m)
                        answer_cb(cq["id"], m if ok else "Колонка недоступна")
                    continue

                msg = upd.get("message") or upd.get("edited_message")
                if not msg:
                    continue
                chat_id = str(msg["chat"]["id"])
                if not allowed_chat():
                    write(ALLOWED, chat_id)
                    send(chat_id, "Доступ привязан к этому чату. /status — состояние.")
                    continue
                if chat_id != allowed_chat():
                    continue
                handle(chat_id, msg.get("text", ""))
        except Exception:
            traceback.print_exc()
            time.sleep(3)


if __name__ == "__main__":
    main()
