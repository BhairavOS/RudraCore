#!/bin/sh
# TLS 1.3 negative tests: servers that misbehave in one way each, and a
# man in the middle that tampers with records. Bhairav OS's tls-test must
# refuse every one, and accept the two controls. Run inside the toolchain
# container after `make` (or: make tls-negative).
#
#   sh scripts/tls-negative.sh <iso> <build-dir> <disk-image>
#
# The servers use a throwaway authority made here; tls-test downloads it
# from http://10.0.2.2:8080/tls/ca.bin (and trusts it only for these tests).
set -eu
ISO="$1"
OUT="$2"
DISK="$3"
T="$OUT/tls-negative"
rm -rf "$T" && mkdir -p "$T/www/tls"
cd "$T"

# MARK: The test authority, and certificates good and bad

cat > ext.cnf <<'EOF'
[ca]
basicConstraints = critical, CA:true
keyUsage = critical, keyCertSign, cRLSign
[notca]
basicConstraints = critical, CA:false
keyUsage = critical, keyCertSign, digitalSignature
[leaf]
basicConstraints = CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:bhairav.test
[other]
basicConstraints = CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:other.test
[client]
basicConstraints = CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = clientAuth
subjectAltName = DNS:bhairav.test
EOF
key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1.key" 2>/dev/null; }
csr() { openssl req -new -key "$1.key" -subj "/CN=$2" -out "$1.csr" 2>/dev/null; }
sign() { # name issuer section [extra x509 options]
    openssl x509 -req -in "$1.csr" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial -days 30 -sha256 \
        -extfile ext.cnf -extensions "$3" -out "$1.crt" ${4:-} 2>/dev/null
}

key ca
openssl req -x509 -new -key ca.key -subj "/CN=Bhairav negative-test CA" -days 30 -sha256 \
    -addext "basicConstraints=critical,CA:true" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt 2>/dev/null
key inter; csr inter "Bhairav test intermediate"; sign inter ca ca
key good; csr good bhairav.test; sign good inter leaf
key other; csr other other.test; sign other inter other
key client; csr client bhairav.test; sign client inter client
key notca; csr notca "Not a CA"; sign notca ca notca
key bynotca; csr bynotca bhairav.test; sign bynotca notca leaf
openssl req -x509 -new -key good.key -subj "/CN=bhairav.test" -days 30 -addext "subjectAltName=DNS:bhairav.test" \
    -out self.crt 2>/dev/null
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out weak.key 2>/dev/null
csr weak bhairav.test; sign weak inter leaf
# Expired: signed with `openssl ca`, which takes explicit dates.
mkdir -p db && : > db/index && echo 01 > db/serial
cat > ca.cnf <<EOF
[ca]
default_ca = test
[test]
database = db/index
serial = db/serial
new_certs_dir = db
default_md = sha256
policy = any
copy_extensions = none
[any]
commonName = supplied
EOF
key expired; csr expired bhairav.test
openssl ca -batch -config ca.cnf -cert inter.crt -keyfile inter.key -in expired.csr -out expired.crt \
    -startdate 20200101000000Z -enddate 20210101000000Z -extfile ext.cnf -extensions leaf -notext 2>/dev/null


# The authority, as a catalog (roots.bin's format) for tls-test.
openssl x509 -in ca.crt -outform DER -out ca.der
"$OLDPWD/$OUT/tools/bpk" catalog www/tls/ca.bin ca.der 2>/dev/null

# MARK: The servers (one scenario per port) and the man in the middle

PIDS=""
serve() { # port cert key chain-or-"-" [options]
    port=$1 cert=$2 k=$3 more=$4
    shift 4
    if [ "$more" != - ]; then set -- -cert_chain "$more" "$@"; fi # (-cert sends only the first certificate)
    openssl s_server -quiet -accept "$port" -cert "$cert" -key "$k" "$@" < /dev/null > "s$port.log" 2>&1 &
    PIDS="$PIDS $!"
}
TLS13="-tls1_3"
serve 8443 good.crt good.key inter.crt $TLS13
serve 8444 other.crt other.key inter.crt $TLS13
serve 8445 expired.crt expired.key inter.crt $TLS13
serve 8446 self.crt good.key - $TLS13
serve 8447 good.crt good.key inter.crt -tls1_2
serve 8448 client.crt client.key inter.crt $TLS13
serve 8449 bynotca.crt bynotca.key notca.crt $TLS13
serve 8450 weak.crt weak.key inter.crt $TLS13 -cipher "DEFAULT:@SECLEVEL=0"
serve 8451 good.crt good.key inter.crt $TLS13 -groups P-256
serve 8452 good.crt good.key inter.crt $TLS13 -verify 1
serve 8453 good.crt good.key inter.crt $TLS13 -ciphersuites TLS_AES_256_GCM_SHA384
serve 9443 good.crt good.key inter.crt $TLS13 -www
python3 "$OLDPWD/scripts/tls-proxy.py" 9443 > proxy.log 2>&1 &
PIDS="$PIDS $!"
trap 'kill $PIDS 2>/dev/null' EXIT
sleep 1
cd "$OLDPWD"

# MARK: Boot, and read tls-test's verdict

WAIT=${WAIT:-40} REPO="$T/www" sh scripts/screenshot.sh "$ISO" "$OUT" "$DISK" > /dev/null 2>&1 || true
grep -a "tls negative check" "$OUT/serial.log" || echo "tls negative check: no result (did tls-test run?)"
grep -aq "tls negative check: FAILED" "$OUT/serial.log" && exit 1
grep -aq "scenarios behaved correctly" "$OUT/serial.log"
