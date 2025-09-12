#!/usr/bin/env bash
set -euo pipefail

DOMAIN="skynet.cikon.eu"
DAYS=10950
CURVE="prime256v1"

openssl ecparam -name "$CURVE" -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days "$DAYS" \
  -subj "/CN=Cikon-CA" -out ca.pem
