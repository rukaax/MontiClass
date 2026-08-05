# TLS root CA bundle

`x509_crt_bundle.bin` is an ESP-IDF 4.4.7 certificate bundle generated from
the system trust store (`/etc/ssl/certs/ca-certificates.crt`). It contains the
certificate subjects and public keys used by `WiFiClientSecure` to verify
public HTTPS servers.
