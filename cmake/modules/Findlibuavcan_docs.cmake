#
# This treats the doxygen build for libuavcan as a standalone program. In
# reality libuavcan_docs is a doxygen build configured just for this project.
#


find_package(Doxygen QUIET)
find_program(PDFLATEX pdflatex)
find_program(MAKEINDEX makeindex)
find_program(EGREP egrep)
find_program(MAKE make)

# +---------------------------------------------------------------------------+
# | DOXYGEN
# +---------------------------------------------------------------------------+

#
# :function: create_docs_target
# Create a target that generates documentation.
#
# :param str ARG_DOCS_TARGET_NAME:  The name to give the target created by this function.
#                                   This is also used as a prefix for sub-targets also
#                                   generated by this function.
# :param bool ARG_ADD_TO_ALL:       If true the target is added to the default build target.
#
function (create_docs_target ARG_DOCS_TARGET_NAME ARG_ADD_TO_ALL)

    set(DOXYGEN_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/doc_source)
    set(DOXYGEN_RDOMAIN org.uavcan)
    set(DOXYGEN_RDOMAIN_W_PROJECT org.uavcan.libuavcan)
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
    set(DOXYGEN_CONFIG_FILE ${DOXYGEN_OUTPUT_DIRECTORY}/doxygen.config)
    set(DOXYGEN_INPUT "\"${CMAKE_CURRENT_SOURCE_DIR}/libuavcan/include\" \"${CMAKE_CURRENT_SOURCE_DIR}/README.md\"")
    set(DOXYGEN_MAINPAGE "\"${CMAKE_CURRENT_SOURCE_DIR}/README.md\"")

    # +-----------------------------------------------------------------------+
    # | HTML (BOOTSTRAPPED)
    # +-----------------------------------------------------------------------+
    set(DOXYGEN_HTML_EXTRA_FILES "${DOXYGEN_SOURCE}/doxygen-bootstrapped/doxy-boot.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/jquery.smartmenus.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/addons/bootstrap/jquery.smartmenus.bootstrap.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/addons/bootstrap/jquery.smartmenus.bootstrap.css")
    set(DOXYGEN_HTML_STYLESHEET ${DOXYGEN_OUTPUT_DIRECTORY}/customdoxygen.css)
    set(DOXYGEN_HTML_HEADER ${DOXYGEN_OUTPUT_DIRECTORY}/header.html)
    set(DOXYGEN_HTML_FOOTER ${DOXYGEN_OUTPUT_DIRECTORY}/footer.html)

    configure_file(${DOXYGEN_SOURCE}/header.html
                    ${DOXYGEN_OUTPUT_DIRECTORY}/header.html
                )
    configure_file(${DOXYGEN_SOURCE}/footer.html
                    ${DOXYGEN_OUTPUT_DIRECTORY}/footer.html
                )
    configure_file(${DOXYGEN_SOURCE}/doxygen-bootstrapped/customdoxygen.css
                    ${DOXYGEN_OUTPUT_DIRECTORY}/customdoxygen.css
                )
    configure_file(${DOXYGEN_SOURCE}/doxygen.ini
                    ${DOXYGEN_CONFIG_FILE}
                )

    add_custom_command(OUTPUT ${DOXYGEN_OUTPUT_DIRECTORY}/html/index.html
                                ${DOXYGEN_OUTPUT_DIRECTORY}/latex/refman.tex
                        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_CONFIG_FILE}
                        DEPENDS ${DOXYGEN_CONFIG_FILE}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        COMMENT "Generating intermediate documentation."
                    )

    add_custom_target(${ARG_DOCS_TARGET_NAME}-html DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/html/index.html)

    if (ARG_ADD_TO_ALL)
        add_custom_target(${ARG_DOCS_TARGET_NAME} ALL DEPENDS ${ARG_DOCS_TARGET_NAME}-html)
    else()
        add_custom_target(${ARG_DOCS_TARGET_NAME} DEPENDS ${ARG_DOCS_TARGET_NAME}-html)
    endif()

    # +-----------------------------------------------------------------------+
    # | PDF
    # +-----------------------------------------------------------------------+
    if (PDFLATEX AND MAKEINDEX AND EGREP AND MAKE)

        message(STATUS "Latex and make found. Will also generate a PDF using doxygen's makefile.")

        add_custom_command(OUTPUT ${DOXYGEN_OUTPUT_DIRECTORY}/latex/refman.pdf
                            COMMAND ${MAKE} pdf
                            DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/latex/refman.tex
                            WORKING_DIRECTORY ${DOXYGEN_OUTPUT_DIRECTORY}/latex
                            COMMENT "Generating refman idx"
                )

        add_custom_target(${ARG_DOCS_TARGET_NAME}-pdf DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/latex/refman.pdf)

        add_dependencies(${ARG_DOCS_TARGET_NAME} ${ARG_DOCS_TARGET_NAME}-pdf)

    else()
        message(STATUS "One or more programs needed to generate PDF documentation was missing. "
                    "Only HTML docs will be generated for this project.")
    endif()

endfunction(create_docs_target)


include(FindPackageHandleStandardArgs)
 
find_package_handle_standard_args(libuavcan_docs
    REQUIRED_VARS DOXYGEN_FOUND
)
