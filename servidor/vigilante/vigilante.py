# -*- coding: utf-8 -*-
"""
Vigilante del riego (Mac o Linux / servidor)
====================================
Lo mantiene vivo launchd (com.riego.vigilante, KeepAlive). Cada 30 s revisa:

  1. Broker Mosquitto: prueba real (publica un ping y espera recibirlo).
     2 fallos seguidos -> lo reinicia (launchctl en Mac, systemctl en Linux).
  2. Web del riego (8081): si no contesta 3 veces -> la reinicia.
     Como mucho 3 reinicios cada 30 min (si sigue fallando, solo avisa).
  3. Placas S3, DevKit, puente C3 y nodo 2 (C3 con WiFi): avisa si se caen, cuando vuelven y por que se reiniciaron
     (corte de corriente, bajada de tension, se colgo y la reinicio su vigilante...).
  4. Corriente del servidor: si el servidor pasa a bateria (corte de luz) y cuando vuelve.
  5. Si el propio servidor se acaba de reiniciar.

Los avisos salen por MQTT en riego/vigilante/aviso (Jarvis los dice y los manda al movil,
la web los guarda en el Historial). Si el broker esta caido, se guardan y se mandan al volver.
Ademas publica riego/vigilante/estado (retenido) para que Jarvis sepa que el vigilante vive.

    venv/bin/python vigilante.py            funcionamiento normal (lo arranca launchd)
    venv/bin/python vigilante.py --prueba   manda un aviso de prueba y termina
"""
import os
import sys
import json
import time
import uuid
import socket
import logging
import platform
import subprocess
import threading
import urllib.request
from collections import deque
from logging.handlers import RotatingFileHandler

import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)
import paho.mqtt.client as mqtt

VERSION = "1.3.0"
CADA_S = 30
HOME = os.path.expanduser("~")
CONFIG_GESTOR = os.path.join(HOME, "riego-inteligente", "gestor", "config.json")
LOG_DIR = os.path.join(HOME, ".riego", "logs")
UID = os.getuid()

# Servicios que vigila: nombre, label de launchd, url de prueba
SERVICIOS = [
    {"nombre": "La web del riego (8081)", "label": "com.riego.web",
     "url": "http://127.0.0.1:8081/login"},
]
LABEL_BROKER = "com.riego.mosquitto"
EN_LINUX = platform.system() == "Linux"
if EN_LINUX:  # en Linux los servicios son de systemd
    _LINUX = {"com.riego.web": "riego-web", "com.riego.mosquitto": "riego-mosquitto"}
    for _s in SERVICIOS:
        _s["label"] = _LINUX[_s["label"]]
    LABEL_BROKER = _LINUX[LABEL_BROKER]

PLACAS = {
    "s3":       {"nombre": "la placa central S3", "online": "riego/s3/online",     "info": "riego/s3/info"},
    "sensores": {"nombre": "la placa de sensores DevKit", "online": None,          "info": "riego/sensores/info"},
    "puente":   {"nombre": "el puente de radio C3", "online": "riego/puente/online", "info": "riego/puente/info"},
    "puente2":  {"nombre": "el nodo 2 con WiFi", "online": "riego/puente2/online", "info": "riego/puente2/info"},
}
PLACA_CADUCA_S = 180          # informan cada 60 s: 3 min sin informe = caida
MAX_REINICIOS, VENTANA_REINICIOS = 3, 1800

MOTIVOS = {
    "encendido": "se ha encendido desde cero: posible CORTE DE CORRIENTE",
    "bajada de tension": "se ha reiniciado por una BAJADA DE TENSION",
    "vigilante (programa colgado)": "se habia colgado y su vigilante la ha reiniciado",
    "fallo del programa": "se ha reiniciado por un fallo del programa",
    "watchdog del sistema": "se ha reiniciado por el watchdog del sistema",
    "reinicio por software": "se ha reiniciado (actualizacion u orden)",
}

def mayus(t):
    return t[:1].upper() + t[1:]


# ------------------------------------------------------------------ log
os.makedirs(LOG_DIR, exist_ok=True)
log = logging.getLogger("vigilante")
log.setLevel(logging.INFO)
_h = RotatingFileHandler(os.path.join(LOG_DIR, "vigilante.log"), maxBytes=1_000_000, backupCount=3, encoding="utf-8")
_h.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
log.addHandler(_h)
if sys.stdout.isatty():          # con launchd la salida ya va al mismo log: no la duplico
    log.addHandler(logging.StreamHandler(sys.stdout))


def cargar_mqtt():
    try:
        with open(CONFIG_GESTOR, encoding="utf-8") as f:
            m = json.load(f).get("mqtt", {})
    except Exception as e:
        log.error(f"No puedo leer {CONFIG_GESTOR}: {e}")
        m = {}
    return m.get("host", "127.0.0.1"), int(m.get("port", 1883)), m.get("user") or None, m.get("pass") or None



def corriente_linux():
    """(enchufado, % bateria) leyendo /sys/class/power_supply. (None, None) si no hay datos.
    Con varias baterias (ThinkPad) calcula el % sobre la energia total."""
    base = "/sys/class/power_supply"
    def leer(p):
        try:
            with open(p) as f:
                return f.read().strip()
        except Exception:
            return None
    try:
        nombres = os.listdir(base)
    except Exception:
        return None, None
    enchufado, ahora, total, pcts = None, 0, 0, []
    for n in nombres:
        tipo = leer(f"{base}/{n}/type")
        if tipo == "Mains":
            if leer(f"{base}/{n}/online") == "1":
                enchufado = True
            elif enchufado is None:
                enchufado = False
        elif tipo == "Battery":
            en, ef = leer(f"{base}/{n}/energy_now"), leer(f"{base}/{n}/energy_full")
            if en and ef and en.isdigit() and ef.isdigit() and int(ef) > 0:
                ahora += int(en); total += int(ef)
            c = leer(f"{base}/{n}/capacity")
            if c and c.isdigit():
                pcts.append(int(c))
    pct = round(100 * ahora / total) if total else (round(sum(pcts) / len(pcts)) if pcts else None)
    return enchufado, pct

class Vigilante:
    def __init__(self, prueba=False):
        self.host, self.port, user, clave = cargar_mqtt()
        self.conectado = False
        self.pendientes = deque(maxlen=50)       # avisos que no se pudieron mandar
        self.ping_id = None
        self.ping_ok = threading.Event()
        self.fallos_broker = 0
        self.fallos = {s["label"]: 0 for s in SERVICIOS}
        self.reinicios = {}                      # label -> [timestamps]
        self.placas = {k: {"online": None, "visto": None, "uptime": None, "caida": False, "desde": None}
                       for k in PLACAS}
        self.encendidas = []                     # (ts, nombre) placas encendidas de cero (para agrupar cortes)
        self.corriente = None                    # True = enchufado, False = bateria
        self.bateria_baja_avisada = False
        self.inicio = time.time()
        self.ultima_conexion = time.time()

        self.cli = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
                               client_id=f"vigilante-{'prueba' if prueba else platform.node().split('.')[0]}")
        if user:
            self.cli.username_pw_set(user, clave)
        if not prueba:
            self.cli.will_set("riego/vigilante/online", "0", qos=1, retain=True)
        self.prueba = prueba
        self.cli.on_connect = self._conectado
        self.cli.on_disconnect = self._desconectado
        self.cli.on_message = self._mensaje
        self.cli.reconnect_delay_set(2, 30)

    # ---------------------------------------------------------- MQTT
    def _conectado(self, cli, userdata, flags, rc):
        if rc != 0:
            log.error(f"Broker rechaza la conexion (rc={rc})")
            return
        self.conectado = True
        self.ultima_conexion = time.time()
        if self.prueba:
            return
        cli.publish("riego/vigilante/online", "1", qos=1, retain=True)
        cli.subscribe([("riego/vigilante/ping", 0), ("riego/+/online", 1), ("riego/+/info", 1)])
        while self.pendientes:
            ts, texto = self.pendientes.popleft()
            hace = int((time.time() - ts) / 60)
            self._publicar_aviso(texto + (f" (hace {hace} min)" if hace >= 1 else ""))

    def _desconectado(self, cli, userdata, rc):
        self.conectado = False

    def _mensaje(self, cli, userdata, msg):
        t = msg.topic
        p = msg.payload.decode("utf-8", "replace").strip()
        if t == "riego/vigilante/ping":
            if p == self.ping_id:
                self.ping_ok.set()
            return
        for k, pl in PLACAS.items():
            if t == pl["online"]:
                self.placas[k]["online"] = (p == "1")
                return
            if t == pl["info"] and not msg.retain:
                try:
                    info = json.loads(p)
                except ValueError:
                    return
                self._info_placa(k, info)
                return

    # ---------------------------------------------------------- avisos
    def _publicar_aviso(self, texto):
        r = self.cli.publish("riego/vigilante/aviso", texto, qos=1)
        return r.rc == mqtt.MQTT_ERR_SUCCESS

    def aviso(self, texto):
        log.warning("AVISO: " + texto)
        if not (self.conectado and self._publicar_aviso(texto)):
            self.pendientes.append((time.time(), texto))

    # ---------------------------------------------------------- placas
    def _info_placa(self, k, info):
        pl, st = PLACAS[k], self.placas[k]
        ahora = time.time()
        up = info.get("uptime_s")
        if st["caida"]:
            mins = int((ahora - (st["desde"] or ahora)) / 60)
            self.aviso(f"{mayus(pl['nombre'])} vuelve a funcionar (estuvo {mins} min sin responder).")
            st["caida"] = False
        # Reinicio: el tiempo encendida baja, o es la primera vez que la veo y lleva poco encendida
        reinicio = up is not None and ((st["uptime"] is not None and up < st["uptime"]) or
                                       (st["uptime"] is None and up < 300 and ahora - self.inicio > 120))
        if reinicio:
            motivo = info.get("arranque", "")
            texto = MOTIVOS.get(motivo, f"se ha reiniciado ({motivo or 'motivo desconocido'})")
            if motivo == "encendido":
                self.encendidas.append((ahora, pl["nombre"]))
            else:
                self.aviso(f"{mayus(pl['nombre'])} {texto}.")
        st["uptime"], st["visto"] = up, ahora

    def _revisar_encendidas(self):
        # Agrupo las placas que se encienden de cero en 3 min: si son varias, es un corte de corriente seguro
        if not self.encendidas or time.time() - self.encendidas[0][0] < 90:
            return
        nombres = sorted({n for _, n in self.encendidas})
        self.encendidas = []
        if len(nombres) > 1:
            self.aviso("CORTE DE CORRIENTE en el riego: se han encendido a la vez " + ", ".join(nombres) +
                       ". Ya ha vuelto la luz. Revisa que las valvulas esten como deben.")
        else:
            self.aviso(f"{mayus(nombres[0])} {MOTIVOS['encendido']}.")

    def _revisar_placas(self):
        ahora = time.time()
        if ahora - self.ultima_conexion < PLACA_CADUCA_S:
            return      # recien conectado al broker: doy tiempo a que las placas informen
        for k, st in self.placas.items():
            sin_info = st["visto"] is None and ahora - self.inicio > PLACA_CADUCA_S
            caducada = st["visto"] is not None and ahora - st["visto"] > PLACA_CADUCA_S
            lwt_caida = st["online"] is False
            if (sin_info or caducada or lwt_caida) and not st["caida"]:
                st["caida"], st["desde"] = True, (st["visto"] or ahora)
                motivo = "ha avisado de que se ha desconectado" if lwt_caida else "lleva 3 minutos sin dar senal"
                self.aviso(f"{mayus(PLACAS[k]['nombre'])} NO RESPONDE: {motivo}. "
                           f"Puede ser corte de corriente, WiFi o averia.")

    # ---------------------------------------------------------- broker y servicios
    def _broker_ok(self):
        if not self.conectado:
            return False
        self.ping_ok.clear()
        self.ping_id = uuid.uuid4().hex[:12]
        self.cli.publish("riego/vigilante/ping", self.ping_id, qos=0)
        return self.ping_ok.wait(10)

    def _puedo_reiniciar(self, label):
        ahora = time.time()
        lista = [t for t in self.reinicios.get(label, []) if ahora - t < VENTANA_REINICIOS]
        self.reinicios[label] = lista
        return len(lista) < MAX_REINICIOS

    def _reiniciar(self, label, nombre):
        if not self._puedo_reiniciar(label):
            self.aviso(f"{nombre} sigue fallando despues de {MAX_REINICIOS} reinicios en 30 min. Hay que mirarlo.")
            return
        self.reinicios[label].append(time.time())
        try:
            cmd = (["sudo", "-n", "systemctl", "restart", f"{label}.service"] if EN_LINUX
                   else ["launchctl", "kickstart", "-k", f"gui/{UID}/{label}"])
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            ok, err = r.returncode == 0, r.stderr.strip()[:120]
        except Exception as e:
            ok, err = False, str(e)[:120]
        if ok:
            self.aviso(f"{nombre} no respondia: reiniciado.")
        else:
            self.aviso(f"{nombre} no responde y no he podido reiniciarlo: {err}")

    def _revisar_broker(self):
        if self._broker_ok():
            self.fallos_broker = 0
            return True
        self.fallos_broker += 1
        log.error(f"Broker sin respuesta ({self.fallos_broker})")
        if self.fallos_broker >= 2:
            self.fallos_broker = 0
            self._reiniciar(LABEL_BROKER, "El broker MQTT (Mosquitto)")
        return False

    def _revisar_servicios(self):
        for s in SERVICIOS:
            try:
                with urllib.request.urlopen(s["url"], timeout=8) as r:
                    ok = r.status == 200
            except Exception:
                ok = False
            if ok:
                self.fallos[s["label"]] = 0
                continue
            self.fallos[s["label"]] += 1
            log.error(f"{s['nombre']} sin respuesta ({self.fallos[s['label']]})")
            if self.fallos[s["label"]] >= 3:
                self.fallos[s["label"]] = 0
                self._reiniciar(s["label"], s["nombre"])

    # ---------------------------------------------------------- corriente del servidor
    def _revisar_corriente(self):
        if EN_LINUX:
            enchufado, pct = corriente_linux()
            if enchufado is None:
                return
        elif platform.system() == "Darwin":
            try:
                out = subprocess.run(["pmset", "-g", "batt"], capture_output=True, text=True, timeout=10).stdout
            except Exception:
                return
            enchufado = "AC Power" in out
            pct = None
            for trozo in out.replace(";", " ").split():
                if trozo.endswith("%") and trozo[:-1].isdigit():
                    pct = int(trozo[:-1])
                    break
        else:
            return
        if self.corriente is not None and enchufado != self.corriente:
            if enchufado:
                self.aviso(f"Ha vuelto la corriente al servidor (bateria al {pct} %).")
                self.bateria_baja_avisada = False
            else:
                self.aviso(f"CORTE DE CORRIENTE en el servidor: el servidor funciona con bateria ({pct} %).")
        if not enchufado and pct is not None and pct <= 20 and not self.bateria_baja_avisada:
            self.bateria_baja_avisada = True
            self.aviso(f"La bateria del servidor esta al {pct} %. Se apagara pronto si no vuelve la luz.")
        self.corriente = enchufado

    def _estado(self, broker_ok):
        est = {"version": VERSION, "ts": int(time.time()), "broker": broker_ok,
               "corriente": self.corriente,
               "placas": {k: (not v["caida"]) for k, v in self.placas.items()},
               "servicios_fallando": [k for k, v in self.fallos.items() if v]}
        if self.conectado:
            self.cli.publish("riego/vigilante/estado", json.dumps(est), qos=1, retain=True)

    # ---------------------------------------------------------- bucle
    def arrancar(self):
        log.info(f"Vigilante {VERSION} arrancando (broker {self.host}:{self.port})")
        self.cli.connect_async(self.host, self.port, 30)
        self.cli.loop_start()
        time.sleep(5)
        try:
            if EN_LINUX:
                with open("/proc/stat") as f:
                    boot = int(next(l for l in f if l.startswith("btime")).split()[1])
            else:
                boot = int(subprocess.run(["sysctl", "-n", "kern.boottime"], capture_output=True, text=True)
                           .stdout.split("sec = ")[1].split(",")[0])
            if time.time() - boot < 900:
                self.aviso("El servidor del riego se acaba de encender o reiniciar "
                           "(si no lo has hecho tu, puede haber sido un corte de corriente).")
        except Exception:
            pass
        while True:
            try:
                ok = self._revisar_broker()
                self._revisar_servicios()
                self._revisar_corriente()
                if ok:
                    self._revisar_placas()
                    self._revisar_encendidas()
                self._estado(ok)
            except Exception as e:
                log.exception(f"Error en la revision: {e}")
            time.sleep(CADA_S)


if __name__ == "__main__":
    v = Vigilante(prueba="--prueba" in sys.argv)
    if v.prueba:
        v.cli.connect(v.host, v.port, 30)
        v.cli.loop_start()
        time.sleep(2)
        v.aviso("Aviso de prueba del vigilante del riego. Si lo oyes, todo funciona.")
        time.sleep(2)
        print("Enviado." if v.conectado else "No he podido conectar con el broker.")
        sys.exit(0)
    v.arrancar()
