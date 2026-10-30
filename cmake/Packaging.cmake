# Install rules and the Debian package (cpack -G DEB).
#
# Layout (prefix /usr):
#   bin/cortrace            Python entry point (runs the cortrace package)
#   bin/cortrace-decode     ETM/DWT/ITM decoder (OpenCSD linked in statically)
#   bin/cortrace-cov        live code coverage for long captures (same OpenCSD)
#   bin/cortrace-prof       cycle-accurate profile from the ETM cycle counts (same OpenCSD)
#   bin/cortrace-grab       FPGA UDP capture helper
#   lib/python3/dist-packages/cortrace/   the Python package
#   share/doc/cortrace/     README, licences
#
# Build the package from a clean tree:
#   cmake -S . -B build-deb -DCMAKE_BUILD_TYPE=Release -DCORTRACE_OPENCSD_STATIC=ON
#   cmake --build build-deb -j && (cd build-deb && cpack -G DEB)

# The version (CORTRACE_VERSION, CORTRACE_DEB_VERSION, _version.py) comes from
# cmake/Version.cmake, which reads the VERSION file at the repository root.

# ---- install ------------------------------------------------------------------
install(TARGETS cortrace-grab RUNTIME DESTINATION bin)
if(CORTRACE_HAVE_OPENCSD AND TARGET cortrace-decode)
    install(TARGETS cortrace-decode RUNTIME DESTINATION bin)
endif()
if(CORTRACE_HAVE_OPENCSD AND TARGET cortrace-cov)
    install(TARGETS cortrace-cov RUNTIME DESTINATION bin)
endif()
if(CORTRACE_HAVE_OPENCSD AND TARGET cortrace-prof)
    install(TARGETS cortrace-prof RUNTIME DESTINATION bin)
endif()
install(PROGRAMS ${CMAKE_SOURCE_DIR}/python/bin/cortrace DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/python/cortrace
        DESTINATION lib/python3/dist-packages
        FILES_MATCHING PATTERN "*.py"
        PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/platforms/
        DESTINATION share/cortrace/platforms
        FILES_MATCHING PATTERN "*.toml")
install(FILES ${CMAKE_BINARY_DIR}/_version.py
        DESTINATION lib/python3/dist-packages/cortrace)
install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/README_en.md
        DESTINATION share/doc/cortrace)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION share/doc/cortrace RENAME copyright)
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/opencsd/LICENSE)
    install(FILES ${CMAKE_SOURCE_DIR}/third_party/opencsd/LICENSE
            DESTINATION share/doc/cortrace RENAME LICENSE.opencsd)
endif()

# ---- CPack: .deb --------------------------------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME cortrace)
set(CPACK_PACKAGE_VERSION "${CORTRACE_DEB_VERSION}")
set(CPACK_PACKAGE_CONTACT "VIFEX <vifextech@foxmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Cortex-M parallel trace (ETM/DWT/ITM) decoder, FPGA capture and Perfetto tools")
set(CPACK_PACKAGE_DESCRIPTION
    "cortrace decodes Cortex-M ETM/DWT/ITM trace captured over a 4/2/1-bit TPIU port into Perfetto traces (call stacks, RTOS thread lanes, software notes on one time axis). It includes the FPGA streamer tools (capture, CSR control, health), a Perfetto Record target (press Start in ui.perfetto.dev) and optional fusion with NuttX nxtrace.")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/cortrace/cortrace")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "python3 (>= 3.10), python3-pyelftools, python3-tomli | python3 (>= 3.11)")
# arm-none-eabi-nm reads the ELF symbols; openocd arms/reads the target.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "binutils-arm-none-eabi")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "openocd")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${CMAKE_SOURCE_DIR}/packaging/debian/postinst
    ${CMAKE_SOURCE_DIR}/packaging/debian/prerm)
include(CPack)
