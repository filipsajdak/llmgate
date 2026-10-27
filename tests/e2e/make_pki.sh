#!/bin/sh
set -eu
dir=${1:?usage: make_pki.sh DIRECTORY}
mkdir -p "$dir"
cd "$dir"
days=36500
authority() {
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.pem" -days "$days" -subj "/CN=$2" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
}
leaf() {
  openssl req -newkey rsa:2048 -nodes -keyout "$2.key" -out "$2.csr" -subj "/CN=origin.test" 2>/dev/null
  printf 'subjectAltName=DNS:origin.test,DNS:localhost,IP:127.0.0.1,IP:192.0.2.7\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > "$2.ext"
  openssl x509 -req -in "$2.csr" -CA "$1.pem" -CAkey "$1.key" -CAcreateserial -out "$2.pem" -days "$days" -extfile "$2.ext" 2>/dev/null
  rm -f "$2.csr" "$2.ext" "$1.srl"
}
authority testca "llmgate test authority"
leaf testca origin
authority corpca "llmgate test corporate authority"
leaf corpca corp
rm -f testca.key corpca.key
