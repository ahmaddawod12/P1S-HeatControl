cmake_minimum_required(VERSION 3.22)
project(p1s_heat_host_tests C)

add_executable(test_device_name test_device_name.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_name.c")
target_include_directories(test_device_name PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include")
target_compile_options(test_device_name PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME device_name COMMAND test_device_name)

add_executable(test_clock_service test_clock_service.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/clock_service.c")
target_include_directories(test_clock_service PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/include")
target_compile_options(test_clock_service PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_clock_service PRIVATE pthread)
add_test(NAME clock_service COMMAND test_clock_service)

add_executable(test_printer_link test_printer_link.c host_freertos.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_link.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_transport.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/printer_report.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/printer_monitor.c")
target_compile_definitions(test_printer_link PRIVATE PRINTER_CA_HOST_TEST=1 HOST_FREERTOS_NO_TIMER=1)
target_include_directories(test_printer_link PRIVATE printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_printer_link PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_printer_link PRIVATE pthread m)
foreach(link_case first_connect connect_timeout subscribe_timeout subscribe_rejected client_id known_state reconnect_waits_for_report report_without_state malformed burst reconnect_counter disconnect network_ap network_got_ip network_lost_ip waiting_burst watching_burst completed_unknown completed_reconnect prior_completion pending_completion unknown_recovery
    started_disconnect started_unknown started_malformed)
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

add_executable(test_app_startup_optional test_app_startup_optional.c
    "${CMAKE_CURRENT_LIST_DIR}/../../main/app_main.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_startup/app_startup.c")
target_include_directories(test_app_startup_optional PRIVATE stubs printer_stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_startup/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/include")
target_compile_definitions(test_app_startup_optional PRIVATE CONFIG_P1S_FIREBASE_ENABLED=1)
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
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/temperature_monitor.c"
)
target_include_directories(test_heat_controller PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/include"
)
target_compile_options(test_heat_controller PRIVATE
    -Wall -Wextra -Werror -pedantic
)
target_link_libraries(test_heat_controller PRIVATE m)

add_test(NAME heat_controller COMMAND test_heat_controller)

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
target_include_directories(test_firmware_trial PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include")
target_compile_options(test_firmware_trial PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firmware_trial COMMAND test_firmware_trial)

add_executable(test_firebase_protocol test_firebase_protocol.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/firebase_protocol.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_firebase_protocol PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_firebase_protocol PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_firebase_protocol PRIVATE m)
add_test(NAME firebase_protocol COMMAND test_firebase_protocol)

add_executable(test_firebase_https test_firebase_https.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/firebase_https.c")
target_include_directories(test_firebase_https PRIVATE firebase_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client")
target_compile_definitions(test_firebase_https PRIVATE CONFIG_P1S_FIREBASE_ENABLED=1)
target_compile_options(test_firebase_https PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME firebase_https COMMAND test_firebase_https)

add_executable(test_firebase_client test_firebase_client.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/firebase_protocol.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_firebase_client PRIVATE firebase_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_link/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_definitions(test_firebase_client PRIVATE HOST_FREERTOS_PTHREAD_CRITICAL CONFIG_P1S_FIREBASE_ENABLED=1
    CONFIG_LWIP_SNTP_STARTUP_DELAY=1 CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY=5000
    CONFIG_P1S_FIREBASE_DATABASE_URL="https://test.firebaseio.com"
    CONFIG_P1S_FIREBASE_API_KEY="test-api-key"
    CONFIG_P1S_FIREBASE_PAIRING_URL="https://example.com/api/pairing")
target_compile_options(test_firebase_client PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_firebase_client PRIVATE pthread m)
add_test(NAME firebase_client COMMAND test_firebase_client)

add_executable(test_firebase_commands test_firebase_commands.c host_freertos.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/firebase_commands.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client/firebase_protocol.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_firebase_commands PRIVATE firebase_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firebase_client"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/clock_service/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/firmware_update/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/printer_control/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_definitions(test_firebase_commands PRIVATE HOST_FREERTOS_PTHREAD_CRITICAL CONFIG_P1S_FIREBASE_ENABLED=1
    CONFIG_P1S_FIREBASE_PUBLIC_RELEASE=1
    CONFIG_P1S_FIREBASE_DATABASE_URL="https://test.firebaseio.com")
target_compile_options(test_firebase_commands PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_firebase_commands PRIVATE pthread m)
add_test(NAME firebase_commands COMMAND test_firebase_commands)
add_test(NAME firebase_command_backpressure COMMAND test_firebase_commands backpressure)
add_test(NAME firebase_monitor_stop_session COMMAND test_firebase_commands monitor_stop_session)
add_test(NAME firebase_disable_token_renewal COMMAND test_firebase_commands token_renewal)
add_test(NAME firebase_disable_stream_reconnect COMMAND test_firebase_commands stream_reconnect)
add_test(NAME firebase_disable_wifi_disconnect COMMAND test_firebase_commands wifi_disconnect)
add_test(NAME firebase_disable_repeated_reconnects COMMAND test_firebase_commands repeated_reconnects)
add_test(NAME firebase_disable_before_boot_restore COMMAND test_firebase_commands boot_stop)
add_test(NAME firebase_initial_snapshot_timeout COMMAND test_firebase_commands initial_snapshot_timeout)
add_test(NAME firebase_disable_with_readings_only COMMAND test_firebase_commands readings_only)
add_test(NAME firebase_enable_wifi_loss COMMAND test_firebase_commands enable_wifi_loss)
add_test(NAME firebase_enable_wifi_reconnect COMMAND test_firebase_commands enable_wifi_reconnect)
add_test(NAME firebase_restart_setting COMMAND test_firebase_commands restart_setting)
add_test(NAME firebase_firmware_update COMMAND test_firebase_commands firmware_update)

add_executable(test_runtime_maintenance test_runtime_maintenance.c host_freertos.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/diagnostics/diagnostics.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/app_runtime.c"
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
add_test(NAME runtime_legacy_mode_lockout COMMAND test_runtime_maintenance legacy_mode_lockout)
add_test(NAME runtime_startup_storage_lockout COMMAND test_runtime_maintenance startup_storage_lockout)
add_test(NAME runtime_durable_lockout_ack COMMAND test_runtime_maintenance durable_lockout_ack)
add_test(NAME runtime_zero_target_relay_failure COMMAND test_runtime_maintenance zero_target_relay_failure)
add_test(NAME runtime_boot_maintenance_save COMMAND test_runtime_maintenance boot_maintenance_save)
add_test(NAME runtime_deferred_relay_disable COMMAND test_runtime_maintenance deferred_relay_disable)
add_test(NAME runtime_boot_off_interval COMMAND test_runtime_maintenance boot_off_interval)
foreach(sensor_case retry_reset retry_stall cadence)
    add_test(NAME runtime_sensor_${sensor_case} COMMAND test_runtime_maintenance ${sensor_case})
endforeach()
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
add_test(NAME runtime_remote_deadlines COMMAND test_runtime_maintenance remote_deadlines)
add_test(NAME runtime_restart_setting COMMAND test_runtime_maintenance restart_setting)
add_test(NAME runtime_remote_enable_failure COMMAND test_runtime_maintenance remote_enable_failure)
add_test(NAME runtime_remote_enable_cancel COMMAND test_runtime_maintenance remote_enable_cancel)
add_test(NAME runtime_remote_enable_expiry COMMAND test_runtime_maintenance remote_enable_expiry)
add_test(NAME runtime_fault_save_power_loss COMMAND test_runtime_maintenance fault_save_power_loss)
add_test(NAME runtime_remote_disable_contention COMMAND test_runtime_maintenance remote_disable_contention)
add_test(NAME runtime_remote_monitor_deadline COMMAND test_runtime_maintenance remote_monitor_deadline)
add_test(NAME runtime_remote_monitor_cancel COMMAND test_runtime_maintenance remote_monitor_cancel)
add_test(NAME runtime_remote_monitor_expiry COMMAND test_runtime_maintenance remote_monitor_expiry)
foreach(monitor_case requires permit disable disable_enable end maintenance stale_end pending_end
    recovery recovery_cancel save_failure save_expiry save_pause)
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
foreach(scenario router association replacement failed_disconnect ip_lost setup_modes setup_save_race replace_while_pairing
    rssi_refresh rssi_rate rssi_failure rssi_other_network rssi_disconnect_race)
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
    ap_mode mac ap_config sta_mode radio_start worker)
    add_test(NAME wifi_startup_${scenario} COMMAND test_wifi_startup ${scenario})
endforeach()

add_executable(test_setup_portal test_setup_portal.c
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/setup_portal.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/http_csrf.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/http_request.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/wifi_api_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/wifi_credentials.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(test_setup_portal PRIVATE web_stubs printer_stubs stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_server"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_manager/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON")
target_compile_options(test_setup_portal PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_setup_portal PRIVATE m)
add_test(NAME setup_portal COMMAND test_setup_portal)

foreach(config_target test_printer_control test_device_config test_wifi_engine test_wifi_manager test_wifi_startup)
    target_sources(${config_target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../components/device_config/device_name.c")
endforeach()
