#!/bin/bash
# =====================================================================
#  Instala el broker Mosquitto como servicio del Mac (puerto de pruebas 1884).
#  Copia la configuracion a ~/.riego/mosquitto (fuera de Syncthing),
#  crea los usuarios y arranca el servicio. No toca el broker antiguo.
#  Se lanza desde la carpeta donde estan los archivos (la copia con scp).
# =====================================================================
set -e
ORIGEN="$(cd "$(dirname "$0")" && pwd)"
DIR="$HOME/.riego/mosquitto"
mkdir -p "$DIR/data"

cp "$ORIGEN/mosquitto-mac.conf" "$DIR/mosquitto.conf"
cp "$ORIGEN/acl" "$DIR/acl"
chmod 600 "$DIR/acl"

echo "== Usuarios"
bash "$ORIGEN/crear_usuarios.sh"

echo "== Servicio"
PL="$HOME/Library/LaunchAgents/com.riego.mosquitto.plist"
launchctl unload "$PL" 2>/dev/null || true
cp "$ORIGEN/com.riego.mosquitto.plist" "$PL"
launchctl load "$PL"
sleep 3
if launchctl list | grep -q com.riego.mosquitto && lsof -nP -iTCP:1884 -sTCP:LISTEN >/dev/null 2>&1; then
  echo "Mosquitto escuchando en el puerto 1884"
else
  echo "ERROR: Mosquitto no arranca. Ultimas lineas del registro:"
  tail -15 "$DIR/mosquitto.log" "$DIR/launchd.err" 2>/dev/null
  exit 1
fi
echo "LISTO"
