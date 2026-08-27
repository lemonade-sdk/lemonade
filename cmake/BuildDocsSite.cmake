# Builds the documentation site with Zensical and stages it for the /docs endpoint.
#
# Zensical always writes to "site/" next to the config, and needs docs/index.md
# (the source page is maintained as docs/README.md), so the tree is copied into a
# staging directory first and the source checkout is left untouched.
#
# Invoked via `cmake -P`; expects SOURCE_DIR, STAGING_DIR, OUTPUT_DIR, ZENSICAL.

foreach(required SOURCE_DIR STAGING_DIR OUTPUT_DIR ZENSICAL)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "BuildDocsSite.cmake: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGING_DIR}")
file(MAKE_DIRECTORY "${STAGING_DIR}")

file(COPY "${SOURCE_DIR}/mkdocs.yml" DESTINATION "${STAGING_DIR}")
file(COPY "${SOURCE_DIR}/docs" DESTINATION "${STAGING_DIR}")
configure_file("${STAGING_DIR}/docs/README.md" "${STAGING_DIR}/docs/index.md" COPYONLY)

execute_process(
    COMMAND "${ZENSICAL}" build --clean
    WORKING_DIRECTORY "${STAGING_DIR}"
    RESULT_VARIABLE zensical_result
)
if(NOT zensical_result EQUAL 0)
    message(FATAL_ERROR "zensical build failed (${zensical_result})")
endif()

if(NOT EXISTS "${STAGING_DIR}/site")
    message(FATAL_ERROR "zensical build produced no site/ directory")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(COPY "${STAGING_DIR}/site/" DESTINATION "${OUTPUT_DIR}")

# Root-absolute URLs make this the one page that cannot be served from /docs/.
file(REMOVE "${OUTPUT_DIR}/404.html")

# Zensical copies non-page files from docs/ verbatim; the domain config and the
# repository's own build scripts have no business inside an install.
file(REMOVE "${OUTPUT_DIR}/CNAME")
file(GLOB_RECURSE DOCS_SITE_SCRIPTS "${OUTPUT_DIR}/*.py")
if(DOCS_SITE_SCRIPTS)
    file(REMOVE ${DOCS_SITE_SCRIPTS})
endif()

message(STATUS "Documentation site staged to ${OUTPUT_DIR}")
