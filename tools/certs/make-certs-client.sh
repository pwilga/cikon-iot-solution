#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <client_name>"
  exit 1
fi

NAME="$1"
DAYS=10950
CURVE="prime256v1"

openssl ecparam -name "$CURVE" -genkey -noout -out ${NAME}.key
openssl req -new -key ${NAME}.key -out ${NAME}.csr -subj "/CN=${NAME}"
openssl x509 -req -in ${NAME}.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out ${NAME}.pem -days "$DAYS" -sha256

rm -f ${NAME}.csr
