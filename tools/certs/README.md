# Certificate Generation System

This directory contains scripts for generating a certificate system for secure communication between devices. The process is based on a Certificate Authority (CA), server, and client certificates.

## Certificate Types and Technical Details

- **CA Certificate**: Root certificate, self-signed. Type: X.509, ECC (prime256v1 curve, a.k.a. secp256r1), validity: 30 years (10950 days).
- **Server Certificate**: Signed by the CA. Type: X.509, ECC (prime256v1), with Subject Alternative Name (SAN) for domain (default: skynet.cikon.eu).
- **Client Certificate**: Signed by the CA. Type: X.509, ECC (prime256v1), CN set to client name.
- **Lightweight Option**: ECC (prime256v1) is used by default in all scripts for best performance and smallest key size for IoT devices.

## How It Works

1. **CA Certificate**: The CA is created first. It is the root of trust for all server and client certificates. The CA must remain unchanged for all devices that need to communicate with each other. If you change the CA, previously generated certificates will no longer be trusted.
2. **Server Certificate**: Generated using the CA. Used by the server to authenticate itself to clients.
3. **Client Certificate**: Generated using the CA. Used by the client to authenticate itself to the server.

## Important Notes
- **Do not change the CA** when generating certificates for devices that need to communicate with each other. All devices must trust the same CA.
- The scripts in this directory automate the process of generating CA, server, and client certificates.

## Usage

### 1. Generate CA Certificate
Run:
```sh
./make-certs.sh ca
```
This will create the CA certificate (`ca.pem`) and key (`ca.key`) files using ECC (prime256v1).

### 2. Generate Server Certificate
Run:
```sh
./make-certs-server.sh <server_name>
```
Replace `<server_name>` with the desired server identifier. This will create a server key (`<server_name>.key`) and certificate (`<server_name>.pem`) signed by the CA, with SAN for domain `skynet.cikon.eu`.

### 3. Generate Client Certificate
Run:
```sh
./make-certs-client.sh <client_name>
```
Replace `<client_name>` with the desired client identifier. This will create a client key (`<client_name>.key`) and certificate (`<client_name>.pem`) signed by the CA.

## Example Workflow
```sh
# Step 1: Create CA
./make-certs.sh

# Step 2: Create server certificate
./make-certs-server.sh myserver

# Step 3: Create client certificate
./make-certs-client.sh myclient
```

## Files Generated
- `ca.pem`, `ca.key`: CA certificate and key
- `<server_name>.pem`, `<server_name>.key`: Server certificate and key
- `<client_name>.pem`, `<client_name>.key`: Client certificate and key

## Summary
Always use the same CA for all devices that need to trust each other. Use the provided scripts to generate server and client certificates as needed. Do not regenerate the CA unless you want to invalidate all existing certificates.

---

