# ./components/adin1110/CMakeLists.txt
idf_component_register(
    SRCS 
        "src/adin_spi.c" 
        "src/adin_mac.c" 
        "src/adin_phy.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        esp_eth 
        esp_netif 
        driver 
        esp_event
        log
)