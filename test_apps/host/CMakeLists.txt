cmake_minimum_required(VERSION 3.22)
project(p1s_heat_host_tests C)

add_executable(test_clock_service test_clock_service.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/clock_service.c")
target_include_directories(test_clock_service PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/include")
target_compile_options(test_clock_service PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_clock_service PRIVATE pthread)
add_test(NAME clock_service COMMAND test_clock_service)

add_executable(test_printer_deadline test_printer_deadline.c printer_stubs/host_transport.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_deadline_transport.c")
target_include_directories(test_printer_deadline PRIVATE printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link")
target_compile_options(test_printer_deadline PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_definitions(test_printer_deadline PRIVATE HOST_TRANSPORT_NO_SOCKET_HELPERS=1)
add_test(NAME printer_deadline COMMAND test_printer_deadline)

add_executable(test_printer_socket test_printer_socket.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_socket.c")
target_include_directories(test_printer_socket PRIVATE socket_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link")
target_compile_options(test_printer_socket PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME printer_socket COMMAND test_printer_socket)

add_executable(test_printer_link test_printer_link.c host_freertos.c printer_stubs/host_transport.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_deadline_transport.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_link.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_transport.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_report.c")
target_compile_definitions(test_printer_link PRIVATE PRINTER_CA_HOST_TEST=1 HOST_FREERTOS_NO_TIMER=1 HOST_FREERTOS_NO_DELAY=1)
target_include_directories(test_printer_link PRIVATE printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_printer_link PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_printer_link PRIVATE pthread m)
foreach(link_case first_connect known_state reconnect_interval report_without_state malformed burst reconnect_counter disconnect network_ap network_got_ip network_lost_ip)
    add_test(NAME printer_link_${link_case} COMMAND test_printer_link ${link_case})
endforeach()

add_executable(test_printer_report test_printer_report.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_report.c")
target_include_directories(test_printer_report PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_printer_report PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_printer_report PRIVATE m)
add_test(NAME printer_report COMMAND test_printer_report)

enable_testing()

add_executable(test_sse_hub test_sse_hub.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/sse_hub.c")
target_include_directories(test_sse_hub PRIVATE sse_stubs web_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include")
target_compile_options(test_sse_hub PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_sse_hub PRIVATE pthread ws2_32)
foreach(sse_case uninitialized init_failure timeout partial header close concurrent)
    add_test(NAME sse_hub_${sse_case} COMMAND test_sse_hub ${sse_case})
endforeach()

add_executable(test_printer_api_policy test_printer_api_policy.c printer_stubs/unused_nvs.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/http_request.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/printer_api.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/printer_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_config.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_printer_api_policy PRIVATE web_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_printer_api_policy PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME printer_api_policy COMMAND test_printer_api_policy)

add_executable(test_app_startup_optional test_app_startup_optional.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_startup/app_startup.c")
target_include_directories(test_app_startup_optional PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_startup/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include")
target_compile_options(test_app_startup_optional PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME app_startup_optional COMMAND test_app_startup_optional)

add_executable(test_firmware_coexistence test_firmware_coexistence.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/service_operation.c")
target_include_directories(test_firmware_coexistence PRIVATE printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include")
target_compile_options(test_firmware_coexistence PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_coexistence COMMAND test_firmware_coexistence)

add_executable(test_printer_control test_printer_control.c host_freertos.c printer_stubs/unused_nvs.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/printer_control.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/printer_monitor.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_config.c")
target_compile_definitions(test_printer_control PRIVATE PRINTER_CONTROL_MANUAL_POLL=1)
target_include_directories(test_printer_control PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include")
target_compile_options(test_printer_control PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_options(test_printer_control PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_printer_control PRIVATE -Wl,--gc-sections)
target_link_libraries(test_printer_control PRIVATE pthread m)
add_test(NAME printer_control COMMAND test_printer_control)

add_executable(test_printer_monitor test_printer_monitor.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/printer_monitor.c")
target_include_directories(test_printer_monitor PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_printer_monitor PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME printer_monitor COMMAND test_printer_monitor)

add_executable(test_settings test_settings.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/settings.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/heat_controller.c")
target_include_directories(test_settings PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include")
target_compile_options(test_settings PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_settings PRIVATE m)
add_test(NAME settings COMMAND test_settings)

add_executable(test_temperature_sensor test_temperature_sensor.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature_sensor/temperature_sensor.c")
target_include_directories(test_temperature_sensor PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature_sensor/include"
    sensor_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_temperature_sensor PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_temperature_sensor PRIVATE m)
add_test(NAME temperature_sensor COMMAND test_temperature_sensor)

add_executable(test_app_types test_app_types.c)
target_include_directories(test_app_types PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
)
target_compile_options(test_app_types PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME app_types_initial_status COMMAND test_app_types)

add_executable(test_temperature_monitor
    test_temperature_monitor.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/temperature_monitor.c"
)
target_include_directories(test_temperature_monitor PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/include"
)
target_compile_options(test_temperature_monitor PRIVATE
    -Wall -Wextra -Werror -pedantic
)
target_link_libraries(test_temperature_monitor PRIVATE m)

add_test(NAME temperature_monitor COMMAND test_temperature_monitor)

add_executable(test_heat_controller
    test_heat_controller.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/heat_controller.c"
)
target_include_directories(test_heat_controller PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
)
target_compile_options(test_heat_controller PRIVATE
    -Wall -Wextra -Werror -pedantic
)
target_link_libraries(test_heat_controller PRIVATE m)

add_test(NAME heat_controller COMMAND test_heat_controller)

add_executable(test_runtime_output_policy
    test_runtime_output_policy.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/runtime_output_policy.c"
)
target_include_directories(test_runtime_output_policy PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime"
)
target_compile_options(test_runtime_output_policy PRIVATE
    -Wall -Wextra -Werror -pedantic
)

add_test(NAME runtime_output_policy COMMAND test_runtime_output_policy)

add_executable(test_firmware_policy
    test_firmware_policy.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_policy.c"
)
target_include_directories(test_firmware_policy PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update"
)
target_compile_options(test_firmware_policy PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_policy COMMAND test_firmware_policy)

add_executable(test_device_config test_device_config.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_config.c")
target_include_directories(test_device_config PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include")
target_compile_options(test_device_config PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME device_config COMMAND test_device_config)

add_executable(test_firmware_manifest test_firmware_manifest.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_manifest.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_firmware_manifest PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_firmware_manifest PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_manifest COMMAND test_firmware_manifest)

add_executable(test_firmware_transfer test_firmware_transfer.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_transfer.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_policy.c")
target_include_directories(test_firmware_transfer PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update")
target_compile_options(test_firmware_transfer PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_transfer COMMAND test_firmware_transfer)

add_executable(test_firmware_job test_firmware_job.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_job.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_policy.c")
target_include_directories(test_firmware_job PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update")
target_compile_options(test_firmware_job PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_job COMMAND test_firmware_job)

add_executable(test_firmware_trial test_firmware_trial.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_trial_policy.c")
target_include_directories(test_firmware_trial PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update")
target_compile_options(test_firmware_trial PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_trial COMMAND test_firmware_trial)

add_executable(test_firmware_api_policy test_firmware_api_policy.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/firmware_api_policy.c")
target_include_directories(test_firmware_api_policy PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server")
target_compile_options(test_firmware_api_policy PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_api_policy COMMAND test_firmware_api_policy)

add_executable(test_runtime_maintenance test_runtime_maintenance.c host_freertos.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/diagnostics/diagnostics.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/app_runtime.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/runtime_output_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/heat_controller.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/temperature_monitor.c")
target_include_directories(test_runtime_maintenance PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/diagnostics/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include")
target_compile_options(test_runtime_maintenance PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_runtime_maintenance PRIVATE pthread m)
add_test(NAME runtime_maintenance COMMAND test_runtime_maintenance)
foreach(persistence_case disable sensor overtemp stale queue failure maintenance)
    add_test(NAME runtime_persistence_${persistence_case}
        COMMAND test_runtime_maintenance save_${persistence_case})
endforeach()
foreach(calibration_case calibration calibration_overtemp)
    add_test(NAME runtime_${calibration_case}
        COMMAND test_runtime_maintenance ${calibration_case})
endforeach()
foreach(ack_case output sensor stale overtemp recovery expired early_timeout changed_fault)
    add_test(NAME runtime_ack_${ack_case} COMMAND test_runtime_maintenance ack_${ack_case})
endforeach()
add_test(NAME runtime_diagnostics COMMAND test_runtime_maintenance diagnostics)
foreach(monitor_case requires permit disable end maintenance)
    add_test(NAME runtime_monitor_${monitor_case}
        COMMAND test_runtime_maintenance monitor_${monitor_case})
endforeach()

add_executable(test_service_operation test_service_operation.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/service_operation.c")
target_include_directories(test_service_operation PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include")
target_compile_options(test_service_operation PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_service_operation PRIVATE pthread)
add_test(NAME service_operation COMMAND test_service_operation)

add_executable(test_wifi_engine test_wifi_engine.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_engine.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c")
target_include_directories(test_wifi_engine PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_wifi_engine PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME wifi_engine COMMAND test_wifi_engine)

add_executable(test_wifi_api_policy test_wifi_api_policy.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/wifi_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_wifi_api_policy PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_wifi_api_policy PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME wifi_api_policy COMMAND test_wifi_api_policy)

add_executable(test_wifi_ap_shutdown test_wifi_ap_shutdown.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_ap_shutdown.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/service_operation.c")
target_include_directories(test_wifi_ap_shutdown PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include")
target_compile_options(test_wifi_ap_shutdown PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME wifi_ap_shutdown COMMAND test_wifi_ap_shutdown)

add_executable(test_wifi_manager test_wifi_manager.c wifi_stubs/unused_startup.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_engine.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_ap_shutdown.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/service_operation.c")
target_include_directories(test_wifi_manager PRIVATE wifi_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
)
target_compile_options(test_wifi_manager PRIVATE -Wall -Wextra -Werror -pedantic)
foreach(scenario router association replacement failed_disconnect ip_lost)
    add_test(NAME wifi_manager_${scenario} COMMAND test_wifi_manager ${scenario})
endforeach()

add_executable(test_wifi_startup test_wifi_startup.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_engine.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_ap_shutdown.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/service_operation.c")
target_include_directories(test_wifi_startup PRIVATE wifi_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include")
target_compile_options(test_wifi_startup PRIVATE -Wall -Wextra -Werror -pedantic)
foreach(scenario none nvs netif event_loop radio_init storage wifi_handler ip_handler
    ap_mode ap_config sta_mode radio_start worker)
    add_test(NAME wifi_startup_${scenario} COMMAND test_wifi_startup ${scenario})
endforeach()

add_executable(test_chamber_http test_chamber_http.c web_stubs/unused_services.c
    printer_stubs/unused_nvs.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/http_request.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/api_contract.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/firmware_api.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/firmware_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/wifi_api.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/wifi_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/printer_api.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/printer_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_config.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/firmware_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/heat_controller.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
set_target_properties(test_chamber_http PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(test_chamber_http PRIVATE web_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/diagnostics/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature_sensor/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_chamber_http PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_chamber_http PRIVATE m)
foreach(chamber_case json calibration origin timeout deadline sse sse_init
    service_rejection service_timeout service_trickle service_final_chunk
    service_disconnect service_nul get_body final_chunk acknowledgement)
    add_test(NAME chamber_http_${chamber_case} COMMAND test_chamber_http ${chamber_case})
endforeach()
