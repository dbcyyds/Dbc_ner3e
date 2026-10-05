# stealth.kpm — the only loadable image
add_kpm_module(stealth
    stealth.c
    ../kfix/kfix.c
    ../hide-maps/hidemaps.c
    ../kload/kload.c
    ../wxshadow/wxshadow.c
    ../wxshadow/wxshadow_pgtable.c
    ../wxshadow/wxshadow_handlers.c
    ../wxshadow/wxshadow_bp.c
    ../wxshadow/wxshadow_scan.c
    ../dbc-rw/dbcrw.c
    ../nr3ehid/nr3ehid.c
    ../kpcloak/kpcloak.c
    ../ktouch/ktouch.c
)

# 子模块以 STEALTH_FUSED 编译：去掉各自的 KPM_INIT/INFO，导出 init/exit
target_compile_definitions(stealth_obj PRIVATE STEALTH_FUSED=1)

target_include_directories(stealth_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../wxshadow
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../hide-maps
    ${CMAKE_CURRENT_SOURCE_DIR}/../kload
    ${CMAKE_CURRENT_SOURCE_DIR}/../dbc-rw
    ${CMAKE_CURRENT_SOURCE_DIR}/../nr3ehid
    ${CMAKE_CURRENT_SOURCE_DIR}/../kfix
    ${CMAKE_CURRENT_SOURCE_DIR}/../kpcloak
    ${CMAKE_CURRENT_SOURCE_DIR}/../ktouch
)
