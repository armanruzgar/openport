add_executable(openport-probe probe/main.cpp)
target_link_libraries(openport-probe PRIVATE openport::providers openport::options)

add_executable(openportd openportd/main.cpp)
target_link_libraries(openportd PRIVATE openport::server openport::providers openport::options openport_build_info)
target_compile_definitions(openportd PRIVATE OPENPORT_VERSION="${PROJECT_VERSION}")

# `cmake --install build --component openport` lays out bin/ and share/openport/web,
# where openportd finds the web terminal beside itself. Build the web first
# (npm run build in web/).
include(GNUInstallDirs)
install(TARGETS openportd openport-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT openport)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/web/dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/openport/web
        COMPONENT openport OPTIONAL)
