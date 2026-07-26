set(DMOD_MODULE_NAME        test_dmnetbridge)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmnetbridge_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# dmarp: checking cache side effects of dmnetbridge_handle_netif_rx()'s
#        opportunistic ARP learning (via dmarp_note_frame()).
target_link_libraries(${DMOD_MODULE_NAME} dmnetbridge_if)

# dmnetif: fixture interfaces are registered directly against it (same
#          "/dev/null"-backed pattern as lib/dmarp/tests/dmarp_test.c).
# dmroute: dmroute_addr_t, used directly to build test addresses.
# dmosi: dmosi_thread_create()/_kill()/_destroy()/_sleep(), used directly by
#        handle_netif_rx_refuses_second_pump_for_same_interface to drive a
#        background pump thread.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmarp
    dmnetif
    dmroute
    dmosi
)
