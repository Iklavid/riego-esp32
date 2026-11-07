#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nodo de humedad SIMULADO (v2, temas de Mosquitto 2026)
=====================================================
Finge ser uno o varios nodos de humedad publicando en riego/sensores/nodoN,
igual que el puente C3. Sirve para probar la web, Jarvis, el vigilante y la S3
sin placas reales.

Por defecto usa los nodos 6, 7 y 8 (libres) para no pisar a los reales (1-5).
La humedad baja poco a poco y sube si la zona asociada esta regando
(lee riego/valvula/N/estado).

Credenciales: usa el usuario 'admin' leyendo ~/.riego/mosquitto/claves.txt
(sin mostrar la clave), o las variables RIEGO_MQTT_USER / RIEGO_MQTT_PASS.

    python3 nodo_simulado.py                 nodos 6,7,8 contra 127.0.0.1
    python3 nodo_simulado.py --nodos 6 --host 192.168.1.100 --cada 10
    python3 nodo_simulado.py --mudo 7        el nodo 7 deja de hablar (prueba de avisos)
"""
import os, sys, time, random, argparse, datetime
import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--nodos", default="6,7,8", help="lista de nodos a simular (1-8)")
ap.add_argument("--cada", type=int, default=30, help="segundos entre lecturas")
ap.add_argument("--mudo", default="", help="nodos que se callan (para probar avisos de nodo sin senal)")
a = ap.parse_args()
NODOS = [int(x) for x in a.nodos.split(",") if x.strip()]
MUDOS = {int(x) for x in a.mudo.split(",") if x.strip()}
ZONA = {6: 2, 7: 3, 8: 4}       # nodo -> valvula cuya apertura lo moja (solo simulacion)


def log(m):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {m}", flush=True)


def credenciales():
    u, p = os.environ.get("RIEGO_MQTT_USER"), os.environ.get("RIEGO_MQTT_PASS")
    if u and p:
        return u, p
    try:
        with open(os.path.expanduser("~/.riego/mosquitto/claves.txt")) as f:
            for linea in f:
                if linea.startswith("admin="):
                    return "admin", linea.split("=", 1)[1].strip()
    except OSError:
        pass
    sys.exit("Sin credenciales: pon RIEGO_MQTT_USER/RIEGO_MQTT_PASS o ejecutalo en el Mac del broker.")


humedad = {n: random.uniform(35, 60) for n in NODOS}
regando = set()


def al_mensaje(c, u, msg):
    v = msg.topic.split("/")[2]
    if v.isdigit():
        (regando.add if msg.payload.decode().upper().startswith("ON") else regando.discard)(int(v))


def al_conectar(c, u, flags, rc, props=None):
    log(f"Conectado a {a.host}:{a.port} (rc={rc})")
    c.subscribe("riego/valvula/+/estado")


cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"nodo-simulado-{os.getpid()}")
cli.username_pw_set(*credenciales())
cli.on_connect = al_conectar
cli.on_message = al_mensaje
cli.connect(a.host, a.port, 30)
cli.loop_start()
log(f"Simulando nodos {NODOS} cada {a.cada} s" + (f" (mudos: {sorted(MUDOS)})" if MUDOS else "") + ". Ctrl+C para parar.")

try:
    while True:
        for n in NODOS:
            moja = ZONA.get(n) in regando
            humedad[n] += (2.0 if moja else -0.3) + random.uniform(-0.5, 0.5)
            humedad[n] = max(3, min(100, humedad[n]))
            if n in MUDOS:
                continue
            cli.publish(f"riego/sensores/nodo{n}", str(round(humedad[n])))
        log("  ".join(f"N{n}={round(humedad[n])}%{' (mudo)' if n in MUDOS else ''}{' regando' if ZONA.get(n) in regando else ''}"
                      for n in NODOS))
        time.sleep(a.cada)
except KeyboardInterrupt:
    log("Parado.")
finally:
    cli.loop_stop()
    cli.disconnect()
