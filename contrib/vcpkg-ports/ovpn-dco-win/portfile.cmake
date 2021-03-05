include(vcpkg_common_functions)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OpenVPN/ovpn-dco-win
    REF 2d0c1e0d12105c8cc28b1f5ef53afe01733310d4
    SHA512 ba794c3025637626200ce3372617f22f0f5188fd380c525848d3fdd0eaada7e59a896281442b88849ec7171052f3ffb454ddd6cb4762255bb5155ebbf76def24
)

file(COPY ${SOURCE_PATH}/uapi/ovpn-dco.h DESTINATION ${CURRENT_PACKAGES_DIR}/include/uapi)

file(INSTALL
    ${SOURCE_PATH}/COPYRIGHT.MIT
    DESTINATION ${CURRENT_PACKAGES_DIR}/share/ovpn-dco-win RENAME copyright)
