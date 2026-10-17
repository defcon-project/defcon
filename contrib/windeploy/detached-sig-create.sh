#!/bin/sh
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
set -eu
OSSLSIGNCODE="${OSSLSIGNCODE:-osslsigncode}"

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <osslcodesign args>"
  echo "example: $0 -key codesign.key"
  exit 1
fi

OUT=signature-win.tar.gz
SRCDIR=unsigned
WORKDIR=$(mktemp -d ./.tmp.XXXXXX)
trap 'rm -rf "$WORKDIR"' 0
trap 'exit 1' HUP INT TERM
OUTDIR="${WORKDIR}/out"
OUTSUBDIR="${OUTDIR}/win"
TIMESERVER=http://timestamp.comodoca.com
CERTFILE="win-codesign.cert"

mkdir -p "${OUTSUBDIR}"
for UNSIGNED_PATH in "${SRCDIR}"/*-unsigned.exe; do
  if [ ! -f "${UNSIGNED_PATH}" ]; then
    echo "No unsigned executables found in ${SRCDIR}" >&2
    exit 1
  fi
  UNSIGNED="${UNSIGNED_PATH##*/}"
  echo "Signing ${UNSIGNED}"
  "${OSSLSIGNCODE}" sign -certs "${CERTFILE}" -t "${TIMESERVER}" -h sha256 -in "${UNSIGNED_PATH}" -out "${WORKDIR}/${UNSIGNED}" "$@"
  if [ ! -s "${WORKDIR}/${UNSIGNED}" ]; then
    echo "Missing signed executable: ${UNSIGNED}" >&2
    exit 1
  fi
  "${OSSLSIGNCODE}" extract-signature -pem -in "${WORKDIR}/${UNSIGNED}" -out "${OUTSUBDIR}/${UNSIGNED}.pem"
  if [ ! -s "${OUTSUBDIR}/${UNSIGNED}.pem" ]; then
    echo "Missing or empty signature: ${UNSIGNED}" >&2
    exit 1
  fi
  rm "${WORKDIR}/${UNSIGNED}"
done

tar -C "${OUTDIR}" -czf "${WORKDIR}/${OUT}" .
mv -f "${WORKDIR}/${OUT}" "${OUT}"
rm -rf "${WORKDIR}"
trap - 0 HUP INT TERM
echo "Created ${OUT}"
