if(NOT MODULE_MOD-PLAYERBOTS STREQUAL "disabled")
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
        "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Ai/World/Rpg")
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
        "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/tests/HumanMovementRulesTest.cpp"
        "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/tests/RoadGraphTest.cpp")
endif()
