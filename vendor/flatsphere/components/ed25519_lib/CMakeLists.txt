idf_component_register(
    SRCS "src/keypair.c"
         "src/sign.c"
         "src/verify.c"
         "src/fe.c"
         "src/ge.c"
         "src/sc.c"
         "src/sha512.c"
    INCLUDE_DIRS "src"
)

# We don't need the seed function (uses /dev/urandom, not available on ESP32)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ED25519_NO_SEED)
