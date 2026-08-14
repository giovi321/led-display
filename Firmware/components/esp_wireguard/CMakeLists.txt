# Vendored esp_wireguard (see PORTING.md). Upstream dropped the ESP-IDF
# component scaffolding when it refocused on ESPHome/PlatformIO, so this
# CMakeLists is written for this project, modeled on trombik/esp_wireguard.
#
# "src" is a public include dir because the public header esp_wireguard.h
# includes esp_wireguard_err.h which lives there.
idf_component_register(
    SRCS
        "src/crypto.c"
        "src/esp_wireguard.c"
        "src/wireguard-platform.c"
        "src/wireguard.c"
        "src/wireguardif.c"
        "src/crypto/refc/blake2s.c"
        "src/crypto/refc/chacha20.c"
        "src/crypto/refc/chacha20poly1305.c"
        "src/crypto/refc/poly1305-donna.c"
        "src/crypto/refc/x25519.c"
    INCLUDE_DIRS "include" "src"
    REQUIRES lwip
    PRIV_REQUIRES esp_netif mbedtls
)
