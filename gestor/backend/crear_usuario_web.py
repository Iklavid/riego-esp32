# -*- coding: utf-8 -*-
"""Crea o cambia un usuario de la web de riego. La contrasena se pide por teclado y no se muestra.
Uso:  venv/bin/python crear_usuario_web.py [usuario]
Se guarda cifrada (PBKDF2-SHA256) en ~/.riego/web/usuarios.json (permisos 600)."""
import os, sys, json, getpass, hashlib, secrets

RUTA = os.environ.get("RIEGO_USUARIOS", os.path.expanduser("~/.riego/web/usuarios.json"))
ITER = 300_000


def main():
    usuario = sys.argv[1] if len(sys.argv) > 1 else input("Usuario: ").strip()
    if not usuario or len(usuario) > 40:
        sys.exit("Usuario no valido")
    c1 = getpass.getpass("Contrasena (min. 10 caracteres): ")
    if len(c1) < 10:
        sys.exit("Demasiado corta, no se ha guardado nada")
    if getpass.getpass("Repitela: ") != c1:
        sys.exit("No coinciden, no se ha guardado nada")
    os.makedirs(os.path.dirname(RUTA), mode=0o700, exist_ok=True)
    try:
        with open(RUTA, encoding="utf-8") as f:
            datos = json.load(f)
    except Exception:
        datos = {}
    sal = secrets.token_bytes(16)
    datos[usuario] = {"sal": sal.hex(), "iter": ITER,
                      "hash": hashlib.pbkdf2_hmac("sha256", c1.encode(), sal, ITER).hex()}
    tmp = RUTA + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(datos, f, indent=1)
    os.chmod(tmp, 0o600)
    os.replace(tmp, RUTA)
    print(f"Usuario '{usuario}' guardado. Reinicia la web para que lo use (no hace falta si ya estaba arrancada).")


if __name__ == "__main__":
    main()
