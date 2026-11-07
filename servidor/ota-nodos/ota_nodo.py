#!/usr/bin/env python3
"""OTA de los nodos de humedad por ESP-NOW (a traves de un puente con WiFi).

Uso (en el servidor, con el venv del gestor):
    gestor/backend/venv/bin/python ota_nodo.py <nodo> <env> [--puente riego/puente2]
    p. ej.  ota_nodo.py 3 nodo3        ota_nodo.py 4 repetidor

1. Compila antes:  cd firmware/nodo-humedad && pio run -e <env>
2. Sirve el firmware por HTTP en el puerto 8080 (abierto en ufw para la red de casa) solo mientras dura.
3. Manda la orden al puente (<puente>/ota_nodo) y va diciendo el estado (<puente>/ota_nodo/estado).
"""
import argparse
import hashlib
import http.server
import json
import os
import socket
import sys
import threading
import time

import paho.mqtt.client as mqtt

HOME = os.path.expanduser("~")
CONFIG_GESTOR = os.path.join(HOME, "riego-inteligente/gestor/config.json")
FIRMWARE = os.path.join(HOME, "Proyectos/Riego/firmware/nodo-humedad/.pio/build/{env}/firmware.bin")
PUERTO_HTTP = 8080
ESPERA_MAX_S = 30 * 60


def ip_local():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def servir(ruta, nombre):
    datos = open(ruta, "rb").read()

    class Manejador(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/" + nombre:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(datos)))
            self.end_headers()
            self.wfile.write(datos)
            print(f"  firmware descargado por {self.client_address[0]}")

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", PUERTO_HTTP), Manejador)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser(description="OTA de un nodo de humedad por ESP-NOW")
    ap.add_argument("nodo", type=int, help="numero de nodo (3, 4 = repetidor...)")
    ap.add_argument("env", help="env de PlatformIO ya compilado (nodo3, repetidor...)")
    ap.add_argument("--puente", default="riego/puente2", help="topic base del puente que lo manda")
    a = ap.parse_args()

    ruta = FIRMWARE.format(env=a.env)
    if not os.path.exists(ruta):
        sys.exit(f"No esta compilado: {ruta}")
    datos = open(ruta, "rb").read()
    md5 = hashlib.md5(datos).hexdigest()
    nombre = f"nodo{a.nodo}.bin"
    url = f"http://{ip_local()}:{PUERTO_HTTP}/{nombre}"
    print(f"Firmware {a.env}: {len(datos)} bytes, md5 {md5}")

    m = json.load(open(CONFIG_GESTOR, encoding="utf-8")).get("mqtt", {})
    fin = threading.Event()
    resultado = {"ok": False}

    def al_mensaje(cli, ud, msg):
        texto = msg.payload.decode("utf-8", "replace")
        print(time.strftime("%H:%M:%S"), texto)
        if texto.startswith("ok"):
            resultado["ok"] = True
            fin.set()
        elif texto.startswith("error"):
            fin.set()

    srv = servir(ruta, nombre)
    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=f"ota-nodo-{os.getpid()}")
    if m.get("user"):
        cli.username_pw_set(m["user"], m.get("pass"))
    cli.on_message = al_mensaje
    cli.connect(m.get("host", "127.0.0.1"), int(m.get("port", 1883)))
    cli.subscribe(a.puente + "/ota_nodo/estado")
    cli.loop_start()
    time.sleep(1)
    cli.publish(a.puente + "/ota_nodo", f"{a.nodo} {url} {len(datos)} {md5}", qos=1)
    print(f"Orden mandada a {a.puente}: nodo {a.nodo}")

    if not fin.wait(ESPERA_MAX_S):
        print("Sin respuesta del puente en 30 min")
    cli.loop_stop()
    srv.shutdown()
    sys.exit(0 if resultado["ok"] else 1)


if __name__ == "__main__":
    main()
