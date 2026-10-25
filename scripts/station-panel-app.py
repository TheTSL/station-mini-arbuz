import json
import os
import time

from flask import Flask, Response, jsonify, request

BASE = "/opt/station-panel"
TOKEN = open(os.path.join(BASE, "token")).read().strip()
STATE_FILE = os.path.join(BASE, "state.json")
MODE_FILE = os.path.join(BASE, "desired_mode")

app = Flask(__name__)


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(data):
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, ensure_ascii=False)
    os.replace(tmp, STATE_FILE)


def load_mode():
    try:
        return open(MODE_FILE).read().strip()
    except Exception:
        return ""


def save_mode(m):
    with open(MODE_FILE, "w") as f:
        f.write(m)


@app.route("/api/report", methods=["POST"])
def report():
    if request.args.get("t") != TOKEN:
        return jsonify(error="forbidden"), 403
    data = request.get_json(force=True, silent=True) or {}
    data["_ts"] = int(time.time())
    save_state(data)
    return jsonify(ok=True)


@app.route("/api/control")
def control():
    if request.args.get("t") != TOKEN:
        return jsonify(error="forbidden"), 403
    return jsonify(desired=load_mode())


@app.route("/api/mode", methods=["POST"])
def set_mode():
    if request.args.get("t") != TOKEN:
        return jsonify(error="forbidden"), 403
    data = request.get_json(force=True, silent=True) or {}
    m = data.get("mode", "")
    if m not in ("alice", "custom"):
        return jsonify(error="bad mode"), 400
    save_mode(m)
    return jsonify(ok=True, mode=m)


@app.route("/api/state")
def state():
    d = load_state()
    d["desired"] = load_mode()
    d["now"] = int(time.time())
    return jsonify(d)


PAGE = """<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<title>РЎС‚Р°РЅС†РёСЏ</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body{background:#0f1216;color:#e6edf3;font-family:system-ui,Segoe UI,Roboto,sans-serif;margin:0;padding:24px}
 h1{font-size:20px;margin:0 0 6px}
 .sub{color:#8b949e;font-size:13px;margin-bottom:18px}
 .row{display:flex;gap:10px;margin-bottom:18px;flex-wrap:wrap}
 button{background:#21262d;color:#e6edf3;border:1px solid #30363d;border-radius:8px;padding:10px 16px;font-size:14px;cursor:pointer}
 button:hover{background:#30363d}
 button.on{border-color:#3fb950;color:#3fb950}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px}
 .card{background:#161b22;border:1px solid #262d36;border-radius:10px;padding:12px}
 .k{color:#8b949e;font-size:11px;text-transform:uppercase;letter-spacing:.05em}
 .v{font-size:19px;margin-top:5px;word-break:break-word}
 .ok{color:#3fb950}.bad{color:#f85149}.warn{color:#d29922}
 .foot{margin-top:16px;color:#8b949e;font-size:12px;display:flex;gap:14px;align-items:center}
 .dot{width:8px;height:8px;border-radius:50%;background:#3fb950;display:inline-block;animation:p 1.4s infinite}
 @keyframes p{0%,100%{opacity:1}50%{opacity:.25}}
</style></head><body>
<h1>Yandex Station Mini 1</h1>
<div class="sub">РњРѕРґРµР»СЊ <b id="model">вЂ”</b>, РїСЂРѕС€РёРІРєР° <b id="ver">вЂ”</b></div>

<div class="row">
  <button id="b_alice" onclick="setMode('alice')">Р РµР¶РёРј РђР»РёСЃС‹</button>
  <button id="b_custom" onclick="setMode('custom')">РЎРІРѕР№ СЂРµР¶РёРј</button>
</div>

<div class="grid" id="g"></div>
<div class="foot"><span class="dot"></span><span id="f">РѕР¶РёРґР°РЅРёРµ РґР°РЅРЅС‹С…вЂ¦</span></div>

<script>
const TOKEN = "__TOKEN__";
const cards = [
  ["Р РµР¶РёРј", "mode"], ["Uptime", "uptime"], ["РЎРІРѕР±РѕРґРЅРѕ RAM", "mem_free"], ["Load", "load"],
  ["platformd", "maind_platformd"], ["mediad", "maind_mediad"],
  ["ADB :5555", "adb"], ["SSH :2222", "ssh"],
  ["Р“СЂРѕРјРєРѕСЃС‚СЊ", "volume"], ["Mute", "mute"],
  ["Wi-Fi", "wifi_ssid"], ["РЎРёРіРЅР°Р»", "wifi_signal"], ["РўРµРјРїРµСЂР°С‚СѓСЂР°", "temp"],
  ["Р”Р°С‚С‡РёРє", "distance"], ["Р–РµСЃС‚-РґРµРјРѕРЅ", "volgest"]
];
function cls(v){
  if (v === true || v === "on") return "ok";
  if (v === false || v === "off") return "bad";
  return "";
}
function render(d){
  document.getElementById("model").textContent = d.model || "вЂ”";
  document.getElementById("ver").textContent = d.version || "вЂ”";
  const g = document.getElementById("g"); g.innerHTML = "";
  for (const [label, key] of cards){
    let v = d[key];
    if (v === undefined || v === null) continue;
    if (typeof v === "boolean") v = v ? "РґР°" : "РЅРµС‚";
    const c = document.createElement("div"); c.className = "card";
    c.innerHTML = `<div class="k">${label}</div><div class="v ${cls(d[key])}">${v}</div>`;
    g.appendChild(c);
  }
  document.getElementById("b_alice").className  = d.mode === "normal" ? "on" : "";
  document.getElementById("b_custom").className = d.mode === "maind-free" ? "on" : "";
  const age = d._ts ? Math.round(Date.now()/1000 - d._ts) : null;
  const des = d.desired ? ` В· Р·Р°РїСЂРѕС€РµРЅРѕ: ${d.desired}` : "";
  document.getElementById("f").textContent =
    (age === null ? "РЅРµС‚ РґР°РЅРЅС‹С…" : `РѕР±РЅРѕРІР»РµРЅРѕ ${age} СЃ РЅР°Р·Р°Рґ`) + des;
}
async function tick(){
  try{
    const r = await fetch("api/state", {cache:"no-store"});
    render(await r.json());
  }catch(e){}
}
async function setMode(m){
  await fetch("api/mode?t=" + encodeURIComponent(TOKEN), {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({mode:m})
  });
  tick();
}
tick(); setInterval(tick, 1000);
</script></body></html>"""


@app.route("/")
def index():
    return Response(PAGE.replace("__TOKEN__", TOKEN), mimetype="text/html")


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8448)

