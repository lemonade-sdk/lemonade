# CMake script to generate WiX locale components
# This script generates WiX XML fragments for all locale files

set(LOCALES_DIR "${CMAKE_CURRENT_BINARY_DIR}/../Release/locales")

if(NOT EXISTS "${LOCALES_DIR}")
    message(WARNING "Locales directory not found: ${LOCALES_DIR}")
    return()
endif()

# Get all .pak files in the locales directory
file(GLOB LOCALE_FILES "${LOCALES_DIR}/*.pak")

# Output file
set(OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/LocaleComponents.wxs.fragment")

# Start writing the XML fragment
file(WRITE "${OUTPUT_FILE}" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
file(APPEND "${OUTPUT_FILE}" "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\">\n")
file(APPEND "${OUTPUT_FILE}" "  <Fragment>\n")
file(APPEND "${OUTPUT_FILE}" "    <ComponentGroup Id=\"LocalesComponents\" Directory=\"LocalesDir\">\n")

# Generate a unique GUID for each locale
set(GUID_BASE "e8f9a0b1-c2d3-4e5f-6a7b-")
set(COUNTER 0)

foreach(LOCALE_FILE ${LOCALE_FILES})
    get_filename_component(LOCALE_NAME ${LOCALE_FILE} NAME)
    string(REPLACE ".pak" "" LOCALE_ID ${LOCALE_NAME})
    string(REPLACE "-" "_" LOCALE_ID ${LOCALE_ID})
    
    # Generate a unique GUID (simplified - in production use proper GUID generation)
    math(EXPR GUID_PART "${COUNTER} + 1000")
    string(SUBSTRING "000000000000${GUID_PART}" -12 12 GUID_SUFFIX)
    
    # Write component
    file(APPEND "${OUTPUT_FILE}" "      <Component Id=\"Locale${LOCALE_ID}\" Guid=\"${GUID_BASE}${GUID_SUFFIX}\">\n")
    file(APPEND "${OUTPUT_FILE}" "        <File Id=\"${LOCALE_ID}.pak\" Source=\"\$(var.SourceDir)\\build\\Release\\locales\\${LOCALE_NAME}\" KeyPath=\"yes\" />\n")
    file(APPEND "${OUTPUT_FILE}" "      </Component>\n")
    
    math(EXPR COUNTER "${COUNTER} + 1")
endforeach()

file(APPEND "${OUTPUT_FILE}" "    </ComponentGroup>\n")
file(APPEND "${OUTPUT_FILE}" "  </Fragment>\n")
file(APPEND "${OUTPUT_FILE}" "</Wix>\n")

message(STATUS "Generated locale components: ${OUTPUT_FILE}")


