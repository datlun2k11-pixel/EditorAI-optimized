# Patch CCKeyboardDispatcher.hpp for iOS SDK 5.10.1 bug
# The codegen generates empty AddressInline_ for constructor/destructor on iOS: GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, ...)
# which expands to `static auto address = ;` -> compile error expected expression
set(PATCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/bindings/bindings/Geode/modify/CCKeyboardDispatcher.hpp")
# Also try alternative path used by some runners
if(NOT EXISTS "${PATCH_FILE}")
  set(PATCH_FILE "${CMAKE_BINARY_DIR}/bindings/bindings/Geode/modify/CCKeyboardDispatcher.hpp")
endif()
if(EXISTS "${PATCH_FILE}")
  file(READ "${PATCH_FILE}" CONTENT)
  string(FIND "${CONTENT}" "GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher" FOUND)
  if(NOT FOUND EQUAL -1)
    string(REPLACE "GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher, )" "// PATCHED IOS: GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR disabled (empty address)" CONTENT "${CONTENT}")
    string(REPLACE "GEODE_APPLY_MODIFY_FOR_DESTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher)" "// PATCHED IOS: GEODE_APPLY_MODIFY_FOR_DESTRUCTOR disabled" CONTENT "${CONTENT}")
    file(WRITE "${PATCH_FILE}" "${CONTENT}")
    message(STATUS "Patched ${PATCH_FILE} for iOS CCKeyboardDispatcher bug")
  endif()
endif()
