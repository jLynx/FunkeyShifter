set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS
        "main.cpp"
        "funkey_ble.cpp"
        "funkey_boot.cpp"
        "funkey_physical.cpp"
        "funkey_protocol.cpp"
        "funkey_report.cpp"
        "funkey_usb.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_adc esp_driver_gpio
)
