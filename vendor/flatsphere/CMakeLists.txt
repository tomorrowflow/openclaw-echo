cmake_minimum_required(VERSION 3.16)

file (STRINGS version.txt BUILD_NUMBER)
set(PROJECT_VER ${BUILD_NUMBER})
add_compile_definitions(BUILD_NUMBER="${BUILD_NUMBER}")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components/microlink/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# PicoTTS needs this
add_compile_options(-Wmaybe-uninitialized)

project(flatsphere)

# Enable lwIP local DNS hostlist so we can map .ts.net hostnames to Tailscale VPN IPs
# (MagicDNS at 100.100.100.100 doesn't work without a local tailscaled daemon)
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PUBLIC
    DNS_LOCAL_HOSTLIST=1
    DNS_LOCAL_HOSTLIST_IS_DYNAMIC=1
    MEMP_NUM_LOCALHOSTLIST=8
)
