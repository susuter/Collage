# Copyright (c) 2010 Daniel Pfeifer <daniel@pfeifer-mail.de>
#               2010-2012 Stefan Eilemann <eile@eyescale.ch>

#info: http://www.itk.org/Wiki/CMake:Component_Install_With_CPack

set(CPACK_PACKAGE_VENDOR "www.eyescale.ch")
set(CPACK_PACKAGE_CONTACT "Stefan Eilemann <eile@eyescale.ch>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Object-Oriented C++ Network Library")
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_SOURCE_DIR}/doc/RELNOTES.md)
set(CPACK_RESOURCE_FILE_README ${CMAKE_SOURCE_DIR}/doc/RELNOTES.md)

set(CPACK_DEBIAN_BUILD_DEPENDS bison flex libboost-system-dev
  libboost-date-time-dev libboost-regex-dev libboost-serialization-dev
  librdmacm-dev libibverbs-dev librdmacm-dev libudt-dev
  ${LUNCHBOX_DEB_DEPENDENCIES})
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6, libboost-system-dev, libboost-date-time-dev, libboost-regex-dev, libboost-serialization-dev, librdmacm-dev, libibverbs-dev, librdmacm-dev, libudt-dev, ${LUNCHBOX_DEB_DEPENDENCIES}")

set(CPACK_MACPORTS_CATEGORY devel)
set(CPACK_MACPORTS_DEPENDS boost Lunchbox)

include(CommonCPack)
