# -- CMAKE for Expressive IDF
# -- author Phil Schatzmann
# -- copyright GPLv3

idf_component_register(
    SRCS "src/ibxm.c"
    INCLUDE_DIRS "src"
)

target_compile_options(${COMPONENT_LIB} PUBLIC "-Wno-unused-variable" "-Wno-missing-field-initializers" "-DCURRENT_ESP_IDF")
