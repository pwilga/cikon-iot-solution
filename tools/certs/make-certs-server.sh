#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <server_name>"
  exit 1
fi

NAME="$1"
DOMAIN="skynet.cikon.eu"
DAYS=10950
CURVE="prime256v1"

openssl ecparam -name "$CURVE" -genkey -noout -out "$NAME.key"

openssl req -new -key "$NAME.key" -out "$NAME.csr" \
  -subj "/CN=${DOMAIN}"

cat > san.cnf <<EOF
[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = ${DOMAIN}
# DNS.2 = mqtt.local
# IP.1  = 192.168.1.10
EOF

openssl x509 -req -in "$NAME.csr" -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out "$NAME.pem" -days "$DAYS" -sha256 \
  -extfile san.cnf -extensions v3_req

rm -f "$NAME.csr" san.cnf
