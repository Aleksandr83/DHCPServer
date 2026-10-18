idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_driver_gpio esp_driver_spi
)

# The init routine keeps a diagnostic read of ESTAT whose value is not logged
# (the register is read a second time for the dump line). The read is part of
# the driver's bring-up diagnostics, so the warning is switched off for this one
# file instead of touching the code.
set_source_files_properties(esp_eth_mac_enc28j60.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable")
