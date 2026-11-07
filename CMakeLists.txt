cmake_minimum_required(VERSION 3.16)
project(icmon VERSION 0.22 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ICMON_BUILD_TESTS "Build the automated tests (use a mock camera that exists ONLY in tests/)" ON)

find_package(Threads REQUIRED)

# Protocol layer: no GUI, no GStreamer. Talks to the camera over DVRIP.
add_library(xmproto STATIC
  src/xm/camtime.cpp
  src/xm/sofia_hash.cpp
  src/xm/json.cpp
  src/xm/dvrip.cpp
  src/xm/archive.cpp)
target_include_directories(xmproto PUBLIC src/xm)
target_compile_options(xmproto PRIVATE -Wall -Wextra)
target_link_libraries(xmproto PUBLIC Threads::Threads)

# Camera-side PTZ presets over ONVIF (no GUI, no extra libraries).
add_library(ptzproto STATIC src/ptz/ptz_onvif.cpp src/ptz/ptz_manager.cpp)
target_include_directories(ptzproto PUBLIC src/ptz)
target_compile_options(ptzproto PRIVATE -Wall -Wextra)
target_link_libraries(ptzproto PUBLIC Threads::Threads)

# Application-level logic without any GUI: camera list file, RTSP address templates.
add_library(icmoncore STATIC src/app/camera_store.cpp src/app/rtsp_url.cpp src/app/discovery.cpp src/app/camctl.cpp)
target_include_directories(icmoncore PUBLIC src/app)
target_link_libraries(icmoncore PUBLIC xmproto)
target_compile_options(icmoncore PRIVATE -Wall -Wextra)

# Console tool on top of the library: list recordings / fetch a time window from the camera's SD card.
add_executable(xmarchive tools/xmarchive.cpp)
target_link_libraries(xmarchive PRIVATE icmoncore xmproto)
target_compile_options(xmarchive PRIVATE -Wall -Wextra)

# xmplay needs GStreamer (appsrc). Skipped with a message when the development files are missing.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
endif()
if(GST_FOUND)
  add_executable(xmplay tools/xmplay.cpp)
  target_link_libraries(xmplay PRIVATE xmproto PkgConfig::GST)
  target_compile_options(xmplay PRIVATE -Wall -Wextra)
else()
  message(STATUS "GStreamer development files not found: xmplay will not be built "
                 "(sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev)")
endif()

# The GTK4 application. Needs: sudo apt install libgtk-4-dev
if(PkgConfig_FOUND)
  pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
endif()
# libsecret (system keyring) is optional: without it, passwords are kept in memory until the program closes.
if(PkgConfig_FOUND)
  pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
endif()
if(GTK4_FOUND AND GST_FOUND)
  add_executable(icmon
    src/app/main.cpp
    src/app/main_window.cpp src/app/settings_dialog.cpp src/app/downloader.cpp
    src/app/camera_dialog.cpp
    src/app/live_player.cpp
    src/app/live_view.cpp
    src/app/playback_view.cpp
    src/app/secrets.cpp
    src/app/timeline.cpp
    src/app/player.cpp)
  # translation catalogs (tools/make_catalogs.py builds them): found by the program next to its executable
  file(COPY ${CMAKE_SOURCE_DIR}/locale DESTINATION ${CMAKE_BINARY_DIR})
  target_include_directories(icmon PRIVATE src/app)
  target_link_libraries(icmon PRIVATE icmoncore xmproto ptzproto PkgConfig::GTK4 PkgConfig::GST)
  if(LIBSECRET_FOUND)
    target_compile_definitions(icmon PRIVATE ICMON_HAVE_LIBSECRET=1)
    target_link_libraries(icmon PRIVATE PkgConfig::LIBSECRET)
  else()
    message(STATUS "libsecret not found: passwords will NOT be saved between runs (sudo apt install libsecret-1-dev)")
  endif()
  target_compile_definitions(icmon PRIVATE ICMON_VERSION="${PROJECT_VERSION}")
  target_compile_options(icmon PRIVATE -Wall -Wextra)
  set_target_properties(icmon PROPERTIES OUTPUT_NAME ICmon)
  # the icon is found next to the executable in the build tree (<exe folder>/icons), else in the installed share/icons
  file(COPY ${CMAKE_SOURCE_DIR}/data/icons DESTINATION ${CMAKE_BINARY_DIR})

  # installation (used by the .deb and the AppImage: packaging/)
  include(GNUInstallDirs)
  install(TARGETS icmon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/locale/ DESTINATION ${CMAKE_INSTALL_DATADIR}/locale FILES_MATCHING PATTERN "*.mo")
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/icons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
  install(FILES ${CMAKE_SOURCE_DIR}/data/org.icmon.client.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

  # .deb: cmake --build build && (cd build && cpack -G DEB)   (or packaging/build-deb.sh)
  set(CPACK_PACKAGE_NAME "icmon")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_CONTACT "ICmon <nanforma@gmail.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Linux client for iCSee-compatible IP cameras")
  set(CPACK_PACKAGE_DESCRIPTION "Live view, playback and download of recordings, PTZ control and saved positions (DVRIP, ONVIF, RTSP).")
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-libav")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gnome-keyring | kwalletmanager, gstreamer1.0-pulseaudio | gstreamer1.0-alsa")
  include(CPack)
else()
  message(STATUS "icmon application not built: it needs both GTK4 and GStreamer development files "
                 "(sudo apt install libgtk-4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev)")
endif()

if(ICMON_BUILD_TESTS)
  enable_testing()
  add_executable(xm_tests tests/test_main.cpp)
  target_include_directories(xm_tests PRIVATE src/app)
  target_link_libraries(xm_tests PRIVATE icmoncore xmproto)
  target_compile_options(xm_tests PRIVATE -Wall -Wextra)
  add_test(NAME xm_tests COMMAND xm_tests)

  # ONVIF client against a TEST-ONLY fake device service that exists only in tests/test_onvif.cpp
  add_executable(onvif_tests tests/test_onvif.cpp)
  target_link_libraries(onvif_tests PRIVATE ptzproto)
  target_compile_options(onvif_tests PRIVATE -Wall -Wextra)
  add_test(NAME onvif_tests COMMAND onvif_tests)

  # Stand-alone TEST-ONLY mock camera, used by tests/e2e_xmarchive.sh to drive the real xmarchive binary.
  add_executable(mock_camera_main tests/mock_camera_main.cpp)
  target_link_libraries(mock_camera_main PRIVATE xmproto)
  add_test(NAME e2e_xmarchive
           COMMAND ${CMAKE_SOURCE_DIR}/tests/e2e_xmarchive.sh $<TARGET_FILE:xmarchive> $<TARGET_FILE:mock_camera_main>)
endif()
