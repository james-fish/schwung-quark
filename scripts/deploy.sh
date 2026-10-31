#!/bin/sh
# Adapted from Amino (MIT). Copy the built package to a Move and install it where Schwung looks for sound generators.
#   scripts/deploy.sh [host]        (default host: $MOVE_HOST, else move.local)
#   MOVE_USER                       (default: whatever ssh picks for the host; as root the module is
#                                    chowned to ableton:users)
#
# Needs an SSH key added at http://move.local/development/ssh. Afterwards rescan modules in
# Schwung Manager (http://move.local:7700) or restart Schwung.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ID=quark
HOST=${1:-${MOVE_HOST:-move.local}}
REMOTE=${MOVE_USER:+$MOVE_USER@}$HOST
PKG="$ROOT/dist/$ID-module.tar.gz"
DEST=/data/UserData/schwung/modules/sound_generators
# Staged on /data, not /tmp: Move's root filesystem (and so /tmp) is nearly full, and staging on
# the module's own filesystem makes the swap a rename.
STAGE=/data/UserData/schwung/.$ID-deploy

[ -f "$PKG" ] || { echo "missing $PKG: run scripts/build.sh first" >&2; exit 1; }

# -4: move.local also advertises IPv6 addresses, and ssh fails trying those first.
SSH_OPTS="-4 -o BatchMode=yes -o ConnectTimeout=10"

echo "== copy to $REMOTE"
ssh $SSH_OPTS "$REMOTE" "rm -rf $STAGE && mkdir -p $STAGE $DEST/$ID"
scp $SSH_OPTS "$PKG" "$REMOTE:$STAGE/$ID-module.tar.gz"

# Each entry is renamed into place rather than overwritten: a running Schwung has dsp.so mapped,
# and truncating it in place would crash the audio thread. Directories (assets/) are replaced
# whole; files the package does not carry (config.json from Schwung Manager) are left alone.
echo "== install into $DEST/$ID"
ssh $SSH_OPTS "$REMOTE" "set -e
  tar -xzf $STAGE/$ID-module.tar.gz -C $STAGE
  for f in $STAGE/$ID/*; do
    t=$DEST/$ID/\$(basename \"\$f\")
    if [ -d \"\$f\" ]; then rm -rf \"\$t\"; fi
    mv -f \"\$f\" \"\$t\"
  done
  rm -rf $STAGE
  if [ \"\$(id -u)\" = 0 ]; then chown -R ableton:users $DEST/$ID; fi
  ls -l $DEST/$ID"
echo "installed. Rescan modules in Schwung Manager or restart Schwung."
