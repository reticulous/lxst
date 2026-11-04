include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
file(GLOB LXST_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
# The device endpoint talks to spangap/audio's engine, which is built only on a
# board with a mic or a speaker (CONFIG_AUDIO_ENGINE); without it the null
# device endpoint stands in behind the same lxstDeviceEndpoint().
if(CONFIG_AUDIO_ENGINE)
    list(REMOVE_ITEM LXST_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/lxst_endpoint_device_null.cpp)
else()
    list(REMOVE_ITEM LXST_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/lxst_endpoint_device.cpp)
endif()
idf_component_register(
    SRCS
        ${LXST_SRCS}
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        ${SPANGAP_REQUIRES}
        json
        esp_timer
)
