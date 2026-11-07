# -*- coding: utf-8 -*-
"""
Web nueva del riego (con login)  -  puerto 8081
================================================
El gestor antiguo (main.py, puerto 8080) se retiro el 07-11-2026.

 - Login con usuario y contrasena (cifrada con PBKDF2 en ~/.riego/web/usuarios.json).
 - Escucha todo riego/# por MQTT y guarda historial en data/historial.db (1 ano).
 - Manda ordenes a la S3 con las mismas reglas que la pantalla:
     V2-V8: maximo 30 min.  V1 y V9: solo boton de averia (cerrar / reabrir).
 - Interruptores de sensores sincronizados con la S3 (riego/sensor/N/activo).
 - Programas en el formato de la S3 (riego/s3/programas/set).

Arranque:  venv/bin/python web.py          (o: uvicorn web:app --host 0.0.0.0 --port 8081)
Usuario:   venv/bin/python crear_usuario_web.py
"""
import os
import json
import time
import hmac
import sqlite3
import hashlib
import logging
import secrets
import threading
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

import paho.mqtt.client as mqtt
from cerebro import Cerebro, CULTIVOS
import uvicorn

VERSION_WEB = "1.2.0"

# ------------------------------------------------------------------ RUTAS
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
GESTOR_DIR = os.path.abspath(os.path.join(BASE_DIR, ".."))
CONFIG_PATH = os.path.join(GESTOR_DIR, "config.json")
WEB_DIR = os.path.join(GESTOR_DIR, "web")
DATA_DIR = os.environ.get("RIEGO_DATA", os.path.join(GESTOR_DIR, "data"))
DB_PATH = os.path.join(DATA_DIR, "historial.db")
USUARIOS_PATH = os.environ.get(
    "RIEGO_USUARIOS", os.path.expanduser("~/.riego/web/usuarios.json"))

PUERTO = int(os.environ.get("RIEGO_WEB_PUERTO", "8081"))


def cargar_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        logging.error(f"No se pudo leer config.json: {e}")
        return {}


CFG = cargar_config()
_m = CFG.get("mqtt", {})
MQTT_HOST = os.environ.get("RIEGO_MQTT_HOST", _m.get("host", "127.0.0.1"))
MQTT_PORT = int(os.environ.get("RIEGO_MQTT_PORT", _m.get("port", 1883)))
MQTT_USER = os.environ.get("RIEGO_MQTT_USER", _m.get("user", "")) or None
MQTT_PASS = os.environ.get("RIEGO_MQTT_PASS", _m.get("pass", "")) or None
MQTT_CLIENT_ID = "gestor-web"          # distinto del gestor antiguo: no se echan entre si

# Nombres iguales que la pantalla de la S3. Se pueden cambiar en config.json -> "web": {"valvulas": {...}}
NOMBRES_V = {
    "1": "Entrada bomba", "2": "Arboles", "3": "Huerto", "4": "Parterre",
    "5": "Parte superior", "6": "Aux 6", "7": "Aux 7", "8": "Aux 8", "9": "Entrada calle",
}
NOMBRES_V.update(CFG.get("web", {}).get("valvulas", {}))
NOMBRES_N = {str(i): f"N{i}" for i in range(1, 9)}
NOMBRES_N.update(CFG.get("web", {}).get("nodos", {}))

MAX_MANUAL_MIN = 30
MAX_PROG_MIN = 120
NODO_CADUCA_S = 2 * 3600

PLACAS = {
    "s3":       {"nombre": "Placa central S3",  "online": "riego/s3/online",       "info": "riego/s3/info"},
    "sensores": {"nombre": "Sensores DevKit",   "online": None,                    "info": "riego/sensores/info"},
    "puente":   {"nombre": "Puente radio C3",   "online": "riego/puente/online",   "info": "riego/puente/info"},
    "puente2":  {"nombre": "Nodo 2 con WiFi",   "online": "riego/puente2/online",  "info": "riego/puente2/info"},
}
INFO_CADUCA_S = 180   # sin info en 3 min -> la doy por caida (la DevKit no tiene aviso de caida)

# ------------------------------------------------------------------ ESTADO EN RAM
LOCK = threading.Lock()
E = {
    "mqtt": False,
    "placas": {k: {"online": None, "info": {}, "visto": None} for k in PLACAS},
    "valvulas": {str(i): {"estado": None, "ts": None} for i in range(1, 10)},
    "nodos": {str(i): {"humedad": None, "visto": None, "activo": True, "version": None, "arranque": None, "via": None}
              for i in range(1, 9)},
    "clima": {"temperatura": None, "humedad": None, "ts": None},
    "lluvia": {"valor": None, "ts": None},
    "deposito": {"nivel": None, "ts": None},
    "inteligente": None,
    "programas": None,
}

# ------------------------------------------------------------------ HISTORIAL (SQLite)
DB_LOCK = threading.Lock()


def db():
    os.makedirs(DATA_DIR, exist_ok=True)
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.execute("PRAGMA journal_mode=WAL")
    return c


def db_iniciar():
    with DB_LOCK, db() as c:
        c.execute("""CREATE TABLE IF NOT EXISTS lecturas(
            ts INTEGER PRIMARY KEY, temp REAL, hum_aire REAL, lluvia INTEGER, deposito INTEGER,
            n1 INTEGER, n2 INTEGER, n3 INTEGER, n4 INTEGER, n5 INTEGER, n6 INTEGER, n7 INTEGER, n8 INTEGER)""")
        c.execute("""CREATE TABLE IF NOT EXISTS eventos(
            id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, origen TEXT, tipo TEXT, texto TEXT)""")
        c.execute("CREATE INDEX IF NOT EXISTS ev_ts ON eventos(ts)")


def evento(origen, tipo, texto):
    try:
        with DB_LOCK, db() as c:
            c.execute("INSERT INTO eventos(ts,origen,tipo,texto) VALUES(?,?,?,?)",
                      (int(time.time()), origen, tipo, texto[:300]))
    except Exception as e:
        logging.error(f"No pude guardar evento: {e}")


def _num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def guardar_lectura():
    ahora = time.time()
    with LOCK:
        fila = [int(ahora),
                _num(E["clima"]["temperatura"]), _num(E["clima"]["humedad"]),
                None if E["lluvia"]["valor"] is None else int(E["lluvia"]["valor"]),
                E["deposito"]["nivel"]]
        for i in range(1, 9):
            n = E["nodos"][str(i)]
            ok = n["humedad"] is not None and n["visto"] and ahora - n["visto"] < NODO_CADUCA_S and n["activo"]
            fila.append(n["humedad"] if ok else None)
    with DB_LOCK, db() as c:
        c.execute("INSERT OR REPLACE INTO lecturas VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", fila)


def bucle_historial():
    ultima_purga = 0
    while True:
        time.sleep(300)
        try:
            guardar_lectura()
            if time.time() - ultima_purga > 86400:
                limite = int(time.time() - 365 * 86400)
                with DB_LOCK, db() as c:
                    c.execute("DELETE FROM lecturas WHERE ts<?", (limite,))
                    c.execute("DELETE FROM eventos WHERE ts<?", (limite,))
                ultima_purga = time.time()
        except Exception as e:
            logging.error(f"Historial: {e}")


# ------------------------------------------------------------------ MQTT
def es_on(p):
    return p.strip().upper().split(" ")[0] in ("ON", "1", "TRUE", "SI")


def on_connect(cli, userdata, flags, rc):
    E["mqtt"] = (rc == 0)
    if rc == 0:
        cli.subscribe("riego/#")
        logging.info("MQTT conectado")
    else:
        logging.error(f"MQTT rechazado (rc={rc})")


def on_disconnect(cli, userdata, rc):
    E["mqtt"] = False


def on_message(cli, userdata, msg):
    t = msg.topic
    try:
        p = msg.payload.decode("utf-8", "replace").strip()
    except Exception:
        return
    ahora = time.time()
    viejo = msg.retain   # los retenidos son el estado guardado, no un cambio nuevo
    with LOCK:
        for k, pl in PLACAS.items():
            if t == pl["online"]:
                antes = E["placas"][k]["online"]
                E["placas"][k]["online"] = (p == "1")
                if antes is not None and antes != (p == "1"):
                    evento(k, "placa", f"{pl['nombre']} {'conectada' if p == '1' else 'CAIDA'}")
                return
            if t == pl["info"]:
                try:
                    E["placas"][k]["info"] = json.loads(p)
                except ValueError:
                    pass
                if not viejo:
                    E["placas"][k]["visto"] = ahora
                return

        if t.startswith("riego/valvula/") and t.endswith("/estado"):
            vid = t.split("/")[2]
            if vid in E["valvulas"]:
                antes = E["valvulas"][vid]["estado"]
                E["valvulas"][vid].update(estado=p, ts=ahora)
                if antes is not None and antes != p and not viejo:
                    evento("valvula", "valvula", f"V{vid} {NOMBRES_V.get(vid, '')}: {p}")
        elif "/info_nodo/nodo" in t or "/via/nodo" in t:
            # Version y arranque de los nodos (MSG_INFO) y por donde llegan (espnow / radio)
            nid = t.rsplit("nodo", 1)[1]
            if nid in E["nodos"]:
                if "/via/" in t:
                    E["nodos"][nid]["via"] = p
                else:
                    try:
                        i = json.loads(p)
                        E["nodos"][nid].update(version=i.get("version"), arranque=i.get("arranque"))
                    except ValueError:
                        pass
        elif t.startswith("riego/sensores/nodo"):
            nid = t[len("riego/sensores/nodo"):]
            if nid in E["nodos"]:
                try:
                    E["nodos"][nid]["humedad"] = max(0, min(100, int(float(p))))
                    if not viejo:
                        E["nodos"][nid]["visto"] = ahora
                except ValueError:
                    pass
        elif t.startswith("riego/sensor/") and t.endswith("/activo"):
            nid = t.split("/")[2]
            if nid in E["nodos"]:
                E["nodos"][nid]["activo"] = es_on(p)
        elif t == "riego/clima/temperatura":
            E["clima"].update(temperatura=p, ts=ahora)
        elif t == "riego/clima/humedad":
            E["clima"].update(humedad=p, ts=ahora)
        elif t == "riego/clima/lluvia":
            antes = E["lluvia"]["valor"]
            E["lluvia"].update(valor=es_on(p), ts=ahora)
            if antes is not None and antes != es_on(p):
                evento("sensores", "clima", "Empieza a llover" if es_on(p) else "Deja de llover")
        elif t == "riego/deposito/nivel":
            try:
                E["deposito"].update(nivel=max(0, min(100, int(float(p)))), ts=ahora)
            except ValueError:
                pass
        elif t == "riego/s3/inteligente":
            E["inteligente"] = es_on(p)
        elif t == "riego/s3/programas":
            try:
                E["programas"] = json.loads(p)
            except ValueError:
                pass
        elif t in ("riego/s3/evento", "riego/sensores/evento", "riego/puente/evento") and not viejo:
            evento(t.split("/")[1], "aviso", p)
        elif t == "riego/vigilante/aviso" and not viejo:
            evento("vigilante", "vigilante", p)


cli = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION1, client_id=MQTT_CLIENT_ID)
if MQTT_USER:
    cli.username_pw_set(MQTT_USER, MQTT_PASS)
cli.on_connect = on_connect
cli.on_disconnect = on_disconnect
cli.on_message = on_message
cli.reconnect_delay_set(2, 30)


def hilo_mqtt():
    while True:
        try:
            cli.connect(MQTT_HOST, MQTT_PORT, 60)
            cli.loop_forever(retry_first_connection=True)
        except Exception as e:
            E["mqtt"] = False
            logging.error(f"MQTT no disponible ({e}); reintento en 10 s")
            time.sleep(10)


def publicar(topic, payload, retain=False):
    if not E["mqtt"]:
        raise HTTPException(503, "Sin conexion con el broker MQTT")
    r = cli.publish(topic, payload, qos=1, retain=retain)
    if r.rc != mqtt.MQTT_ERR_SUCCESS:
        raise HTTPException(503, "No se pudo enviar la orden")


# ------------------------------------------------------------------ LOGIN
SESIONES = {}            # token -> {"usuario", "caduca"}
SESION_S = 12 * 3600
FALLOS = {}              # ip -> [n_fallos, bloqueado_hasta]
MAX_FALLOS, BLOQUEO_S = 5, 300
COOKIE = "riego_sesion"


def leer_usuarios():
    try:
        with open(USUARIOS_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def comprobar_clave(usuario, clave):
    u = leer_usuarios().get(usuario)
    if not u:
        # mismo tiempo de calculo exista o no el usuario
        hashlib.pbkdf2_hmac("sha256", clave.encode(), b"x" * 16, 300_000)
        return False
    calc = hashlib.pbkdf2_hmac("sha256", clave.encode(), bytes.fromhex(u["sal"]), int(u["iter"]))
    return hmac.compare_digest(calc.hex(), u["hash"])


def usuario_de(request: Request):
    tok = request.cookies.get(COOKIE)
    s = SESIONES.get(tok) if tok else None
    if not s or s["caduca"] < time.time():
        if tok:
            SESIONES.pop(tok, None)
        return None
    return s["usuario"]


ABIERTAS = {"/login", "/api/login", "/web/app.css", "/favicon.ico"}


# ------------------------------------------------------------------ APP
@asynccontextmanager
async def lifespan(app):
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
    db_iniciar()
    threading.Thread(target=hilo_mqtt, daemon=True).start()
    threading.Thread(target=bucle_historial, daemon=True).start()
    threading.Thread(target=CEREBRO.bucle, daemon=True).start()
    if not leer_usuarios():
        logging.warning("No hay usuarios: ejecuta crear_usuario_web.py")
    logging.info(f"Web de riego {VERSION_WEB} en puerto {PUERTO}, broker {MQTT_HOST}:{MQTT_PORT}")
    yield
    try:
        cli.disconnect()
    except Exception:
        pass


app = FastAPI(title="Riego inteligente", version=VERSION_WEB, lifespan=lifespan,
              docs_url=None, redoc_url=None, openapi_url=None)


@app.middleware("http")
async def seguridad(request: Request, call_next):
    ruta = request.url.path
    if ruta not in ABIERTAS:
        if not usuario_de(request):
            if ruta.startswith("/api/"):
                return JSONResponse({"detail": "Inicia sesion"}, status_code=401)
            return RedirectResponse("/login")
        # Ordenes: solo desde la propia web (cabecera que otra web no puede poner sin permiso)
        if request.method != "GET" and request.headers.get("x-riego") != "1":
            return JSONResponse({"detail": "Peticion no permitida"}, status_code=403)
    resp = await call_next(request)
    resp.headers["X-Frame-Options"] = "DENY"
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["Referrer-Policy"] = "same-origin"
    resp.headers["Cache-Control"] = "no-store"
    return resp


@app.post("/api/login")
async def login(request: Request):
    ip = request.client.host if request.client else "?"
    n, hasta = FALLOS.get(ip, [0, 0])
    if hasta > time.time():
        raise HTTPException(429, f"Demasiados intentos. Espera {int(hasta - time.time())} s")
    try:
        d = await request.json()
    except Exception:
        raise HTTPException(400, "Datos incorrectos")
    usuario, clave = str(d.get("usuario", ""))[:40], str(d.get("clave", ""))[:200]
    if not comprobar_clave(usuario, clave):
        n += 1
        FALLOS[ip] = [0, time.time() + BLOQUEO_S] if n >= MAX_FALLOS else [n, 0]
        evento("web", "login", f"Login fallido desde {ip} ({usuario})")
        raise HTTPException(401, "Usuario o contrasena incorrectos")
    FALLOS.pop(ip, None)
    tok = secrets.token_urlsafe(32)
    SESIONES[tok] = {"usuario": usuario, "caduca": time.time() + SESION_S}
    evento("web", "login", f"Entra {usuario} desde {ip}")
    r = JSONResponse({"ok": True})
    r.set_cookie(COOKIE, tok, max_age=SESION_S, httponly=True, samesite="strict")
    return r


@app.post("/api/logout")
def logout(request: Request):
    SESIONES.pop(request.cookies.get(COOKIE, ""), None)
    r = JSONResponse({"ok": True})
    r.delete_cookie(COOKIE)
    return r


@app.get("/api/estado")
def api_estado(request: Request):
    ahora = time.time()
    with LOCK:
        d = json.loads(json.dumps(E))
    for k, p in d["placas"].items():
        p["nombre"] = PLACAS[k]["nombre"]
        reciente = p["visto"] is not None and ahora - p["visto"] < INFO_CADUCA_S
        if p["online"] is None:          # placa sin aviso de caida: miro si informa
            p["online"] = reciente
        elif p["online"] and not reciente and p["visto"] is not None:
            p["online"] = False
        p["hace_s"] = None if p["visto"] is None else int(ahora - p["visto"])
    for k, v in d["valvulas"].items():
        v["nombre"] = NOMBRES_V.get(k, f"V{k}")
        v["abierta"] = None if v["estado"] is None else es_on(v["estado"])
        v["auto"] = bool(v["estado"]) and "AUTO" in v["estado"].upper()
    for k, n in d["nodos"].items():
        n["nombre"] = NOMBRES_N.get(k, f"N{k}")
        n["al_dia"] = n["visto"] is not None and ahora - n["visto"] < NODO_CADUCA_S
        n["hace_s"] = None if n["visto"] is None else int(ahora - n["visto"])
    d["usuario"] = usuario_de(request)
    d["version_web"] = VERSION_WEB
    d["limites"] = {"manual_min": MAX_MANUAL_MIN, "prog_min": MAX_PROG_MIN}
    d["ts"] = ahora
    return d


@app.post("/api/valvula/{vid}")
async def api_valvula(vid: int, request: Request):
    """Zonas 2-8: {"accion":"abrir","minutos":N} o {"accion":"cerrar"}."""
    if not 2 <= vid <= 8:
        raise HTTPException(400, "Las valvulas 1 y 9 solo se manejan con el boton de averia")
    d = await request.json()
    u = usuario_de(request)
    if d.get("accion") == "abrir":
        minutos = max(1, min(MAX_MANUAL_MIN, int(d.get("minutos", MAX_MANUAL_MIN))))
        publicar(f"riego/valvula/{vid}/set", str(minutos))
        evento("web", "orden", f"{u} abre V{vid} {NOMBRES_V[str(vid)]} {minutos} min")
    elif d.get("accion") == "cerrar":
        publicar(f"riego/valvula/{vid}/set", "OFF")
        evento("web", "orden", f"{u} cierra V{vid} {NOMBRES_V[str(vid)]}")
    else:
        raise HTTPException(400, "Accion no valida")
    return {"ok": True}


@app.post("/api/averia/{vid}")
async def api_averia(vid: int, request: Request):
    """V1 (entrada bomba) y V9 (entrada calle): {"cerrar": true} por averia, {"cerrar": false} reabre."""
    if vid not in (1, 9):
        raise HTTPException(400, "Solo V1 y V9")
    d = await request.json()
    cerrar = bool(d.get("cerrar"))
    publicar(f"riego/valvula/{vid}/set", "OFF" if cerrar else "ON")
    evento("web", "averia", f"{usuario_de(request)} {'CIERRA por averia' if cerrar else 'reabre'} "
                            f"V{vid} {NOMBRES_V[str(vid)]}")
    return {"ok": True}


@app.post("/api/sensor/{nid}")
async def api_sensor(nid: int, request: Request):
    if not 1 <= nid <= 8:
        raise HTTPException(400, "Sensor no valido")
    d = await request.json()
    activo = bool(d.get("activo"))
    publicar(f"riego/sensor/{nid}/activo", "1" if activo else "0", retain=True)
    with LOCK:
        E["nodos"][str(nid)]["activo"] = activo
    evento("web", "sensor", f"{usuario_de(request)} {'activa' if activo else 'desactiva'} sensor N{nid}")
    return {"ok": True}


@app.post("/api/inteligente")
async def api_inteligente(request: Request):
    d = await request.json()
    on = bool(d.get("on"))
    publicar("riego/s3/inteligente/set", "ON" if on else "OFF")
    evento("web", "orden", f"{usuario_de(request)} {'activa' if on else 'desactiva'} riego inteligente")
    return {"ok": True}


@app.post("/api/programas")
async def api_programas(request: Request):
    """{"zonas": {"2": [{"h":6,"m":0,"min":10,"dias":127}, x3], ...}} -> la S3 lo guarda y lo devuelve."""
    d = await request.json()
    zonas = d.get("zonas")
    if not isinstance(zonas, dict):
        raise HTTPException(400, "Falta 'zonas'")
    limpio = {}
    for z, turnos in zonas.items():
        if not (z.isdigit() and 2 <= int(z) <= 8) or not isinstance(turnos, list):
            raise HTTPException(400, f"Zona no valida: {z}")
        lt = []
        for t in turnos[:3]:
            h, m, mi, di = int(t.get("h", 6)), int(t.get("m", 0)), int(t.get("min", 0)), int(t.get("dias", 127))
            if not (0 <= h <= 23 and 0 <= m <= 59 and 0 <= mi <= MAX_PROG_MIN and 0 <= di <= 127):
                raise HTTPException(400, f"Turno no valido en zona {z}")
            lt.append({"h": h, "m": m, "min": mi, "dias": di})
        while len(lt) < 3:
            lt.append({"h": 6, "m": 0, "min": 0, "dias": 127})
        limpio[z] = lt
    publicar("riego/s3/programas/set", json.dumps({"zonas": limpio}, separators=(",", ":")))
    evento("web", "programa", f"{usuario_de(request)} envia programas ({len(limpio)} zonas)")
    return {"ok": True}


@app.get("/api/historial")
def api_historial(horas: int = 24):
    horas = max(1, min(24 * 365, horas))
    desde = int(time.time() - horas * 3600)
    paso = max(300, (horas * 3600) // 400)     # como mucho ~400 puntos por grafica
    cols = "temp,hum_aire,lluvia,deposito,n1,n2,n3,n4,n5,n6,n7,n8"
    with DB_LOCK, db() as c:
        filas = c.execute(
            f"SELECT (ts/{paso})*{paso} AS t, " +
            ",".join(f"AVG({x})" for x in cols.split(",")) +
            " FROM lecturas WHERE ts>=? GROUP BY t ORDER BY t", (desde,)).fetchall()
    return {"columnas": ["ts"] + cols.split(","), "filas": filas, "paso_s": paso}


@app.get("/api/eventos")
def api_eventos(limite: int = 200, tipo: str = ""):
    limite = max(1, min(2000, limite))
    with DB_LOCK, db() as c:
        if tipo:
            filas = c.execute("SELECT ts,origen,tipo,texto FROM eventos WHERE tipo=? ORDER BY id DESC LIMIT ?",
                              (tipo, limite)).fetchall()
        else:
            filas = c.execute("SELECT ts,origen,tipo,texto FROM eventos ORDER BY id DESC LIMIT ?",
                              (limite,)).fetchall()
    return [{"ts": f[0], "origen": f[1], "tipo": f[2], "texto": f[3]} for f in filas]


# ------------------------------------------------------------------ RIEGO INTELIGENTE
def _nodos_para_cerebro():
    ahora = time.time()
    with LOCK:
        nodos = json.loads(json.dumps(E["nodos"]))
    for n in nodos.values():
        n["al_dia"] = n["visto"] is not None and ahora - n["visto"] < NODO_CADUCA_S
    return {"nodos": nodos}


def _llovio(fecha):
    """True/False si el sensor de lluvia marco lluvia ese dia; None si no hay lecturas."""
    ini = int(time.mktime(time.strptime(fecha, "%Y-%m-%d")))
    with DB_LOCK, db() as c:
        f = c.execute("SELECT COUNT(lluvia), MAX(lluvia) FROM lecturas WHERE ts>=? AND ts<?",
                      (ini, ini + 86400)).fetchone()
    return None if not f or not f[0] else bool(f[1])


CEREBRO = Cerebro(DATA_DIR, os.path.expanduser("~/.riego/web/aemet.key"),
                  _nodos_para_cerebro, _llovio, publicar, evento)


@app.get("/api/cerebro")
def api_int_get():
    cfg = CEREBRO.config()
    return {"config": cfg, "cultivos": {k: v["nombre"] for k, v in CULTIVOS.items()},
            "aemet_clave_puesta": CEREBRO.clave_aemet() is not None,
            "aprendizaje": CEREBRO.aprendizaje(), "ultimo": CEREBRO.ultimo,
            "s3_inteligente": E["inteligente"]}


@app.post("/api/cerebro")
async def api_int_set(request: Request):
    try:
        cfg = CEREBRO.guardar_config(await request.json())
    except (ValueError, TypeError) as e:
        raise HTTPException(400, str(e))
    evento("web", "orden", f"{usuario_de(request)} cambia la configuracion del riego inteligente (modo {cfg['modo']})")
    return {"ok": True, "config": cfg}


@app.post("/api/cerebro/aemet")
async def api_int_aemet(request: Request):
    d = await request.json()
    try:
        CEREBRO.guardar_clave_aemet(d.get("clave", ""))
    except ValueError as e:
        raise HTTPException(400, str(e))
    evento("web", "orden", f"{usuario_de(request)} cambia la clave de AEMET")
    return {"ok": True, "puesta": CEREBRO.clave_aemet() is not None}


@app.get("/api/cerebro/buscar")
def api_int_buscar(q: str):
    if len(q.strip()) < 2:
        return []
    try:
        return CEREBRO.buscar_lugar(q.strip()[:80])
    except Exception as e:
        raise HTTPException(502, f"No se pudo buscar: {e}")


@app.post("/api/cerebro/calcular")
async def api_int_calcular(request: Request):
    """{"real": false} = simulacion (no aprende, no apunta, no envia). {"real": true} = calculo del dia."""
    d = await request.json()
    real = bool(d.get("real"))
    try:
        r = CEREBRO.calcular(aprender=real)
    except ValueError as e:
        raise HTTPException(400, str(e))
    except Exception as e:
        raise HTTPException(502, f"No se pudo calcular: {e}")
    if real:
        evento("web", "orden", f"{usuario_de(request)} lanza el calculo del riego inteligente")
    return r


@app.get("/api/cerebro/decisiones")
def api_int_decisiones(dias: int = 30):
    return CEREBRO.decisiones(max(1, min(365, dias)))


# ------------------------------------------------------------------ PAGINAS
@app.get("/login")
def pag_login():
    return FileResponse(os.path.join(WEB_DIR, "login.html"))


@app.get("/")
def pag_inicio():
    return FileResponse(os.path.join(WEB_DIR, "index.html"))


if os.path.isdir(WEB_DIR):
    app.mount("/web", StaticFiles(directory=WEB_DIR), name="web")


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=PUERTO, proxy_headers=False)
