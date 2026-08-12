# lwIP M2 source allowlist. Keep protocol selection explicit: no TCP, IPv6,
# PPP, applications, or sequential/socket API objects enter netd.
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/third_party/lwip)
set(LWIP_M2_SOURCES
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/acd.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/netif/ethernet.c)

add_third_party_lib(lwip STATIC C
    SOURCES ${LWIP_M2_SOURCES}
    INCLUDE_DIRS
        ${LWIP_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/user/netd/port/include
    INTERFACE_INCLUDE_DIRS
        ${LWIP_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/user/netd/port/include)

