#!/usr/bin/env bash
# Creates a throwaway CA and one server certificate for every venue host.
# The bot trusts the CA only through SSL_CERT_FILE in the test shell.
set -euo pipefail
cd "$(dirname -- "${BASH_SOURCE[0]}")"
mkdir -p certs
cd certs
cat > san.cnf <<'EOF'
[ext]
subjectAltName=DNS:clob.polymarket.com,DNS:gamma-api.polymarket.com,DNS:ws-subscriptions-clob.polymarket.com,DNS:ws-feed.exchange.coinbase.com,DNS:polygon-bor-rpc.publicnode.com,DNS:ws-live-data.polymarket.com,DNS:fstream.binance.com,DNS:stream.binance.com
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
EOF
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 30 \
  -subj "/CN=Sniper Fake Venue Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=sniper-fake-venue" 2>/dev/null
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 30 -extfile san.cnf -extensions ext 2>/dev/null
openssl verify -CAfile ca.crt server.crt
