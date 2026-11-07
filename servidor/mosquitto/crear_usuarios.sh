#!/bin/bash
# =====================================================================
#  Crea los usuarios del broker Mosquitto con contrasenas aleatorias.
#  - Las guarda en ~/.riego/mosquitto/claves.txt (solo legible por ti).
#  - NO muestra ninguna contrasena en pantalla.
#  - Si un usuario ya existe, NO le cambia la contrasena (salvo --rehacer).
# =====================================================================
set -e
export PATH=$HOME/.riego/mosquitto-bin/bin:$HOME/.riego/mosquitto-bin/sbin:/usr/local/bin:/usr/local/sbin:$PATH

DIR="$HOME/.riego/mosquitto"
PASSWD="$DIR/passwd"
CLAVES="$DIR/claves.txt"
USUARIOS="s3 sensores puente gestor jarvis admin"

mkdir -p "$DIR/data"
chmod 700 "$HOME/.riego" "$DIR"
touch "$PASSWD" "$CLAVES"
chmod 600 "$PASSWD" "$CLAVES"

if [ "$1" = "--rehacer" ]; then : > "$PASSWD"; : > "$CLAVES"; fi

for u in $USUARIOS; do
  if grep -q "^$u:" "$PASSWD"; then
    echo "  $u: ya existe, no se toca"
    continue
  fi
  clave=$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 24)
  mosquitto_passwd -b "$PASSWD" "$u" "$clave"
  echo "$u=$clave" >> "$CLAVES"
  echo "  $u: creado"
done
unset clave

echo
echo "Usuarios del broker: $(cut -d: -f1 "$PASSWD" | tr '\n' ' ')"
echo "Las contrasenas estan en $CLAVES (solo tu usuario puede leerlo)."
