cmake_minimum_required(VERSION 3.22)
project(p1s_heat_host_tests C)

enable_testing()

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
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/app_runtime.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/runtime_output_policy.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/heat_controller.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/temperature_monitor.c")
target_include_directories(test_runtime_maintenance PRIVATE stubs
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings/include")
target_compile_options(test_runtime_maintenance PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_runtime_maintenance PRIVATE pthread m)
add_test(NAME runtime_maintenance COMMAND test_runtime_maintenance)

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
foreach(scenario router association replacement failed_disconnect)
    add_test(NAME wifi_manager_${scenario} COMMAND test_wifi_manager ${scenario})
endforeach()
