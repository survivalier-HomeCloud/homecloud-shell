#!/bin/sh
# HomeCloud Shell - compile, installe le client et le serveur puis lance l'assistant.
#   Édition complète : sudo sh install.sh        Termux / édition lite : sh install.sh
set -e
cd "$(dirname "$0")"
command -v make >/dev/null 2>&1 && { command -v cc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; } || {
  echo "Il faut un compilateur C et make."
  echo "  Debian/Ubuntu : sudo apt install build-essential"
  echo "  Termux        : pkg install clang make"
  exit 1; }
if [ -n "$PREFIX" ] && [ -d "$PREFIX/bin" ] && echo "$PREFIX" | grep -q termux; then DEST="$PREFIX"
elif [ "$(id -u)" = 0 ]; then DEST=/usr/local
else DEST="$HOME/.local"; fi
make -s
install -Dm755 homecloud-shell "$DEST/bin/homecloud-shell" 2>/dev/null || cp homecloud-shell "$DEST/bin/homecloud-shell"
install -Dm755 homecloud-shelld "$DEST/bin/homecloud-shelld" 2>/dev/null || { mkdir -p "$DEST/bin" && cp homecloud-shelld "$DEST/bin/homecloud-shelld" && chmod 755 "$DEST/bin/homecloud-shelld"; }
case ":$PATH:" in *":$DEST/bin:"*) ;; *) echo "Ajoutez $DEST/bin à votre PATH pour utiliser « homecloud-shelld »." ;; esac
exec "$DEST/bin/homecloud-shelld" install
