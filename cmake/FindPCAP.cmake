# Minimal FindPCAP module — wraps the system libpcap installation.
find_path(PCAP_INCLUDE_DIR pcap.h
    HINTS /usr/include /usr/local/include)
find_library(PCAP_LIBRARY NAMES pcap
    HINTS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PCAP DEFAULT_MSG PCAP_LIBRARY PCAP_INCLUDE_DIR)

if(PCAP_FOUND)
    set(PCAP_LIBRARIES    ${PCAP_LIBRARY})
    set(PCAP_INCLUDE_DIRS ${PCAP_INCLUDE_DIR})

    if(NOT TARGET PCAP::PCAP)
        add_library(PCAP::PCAP UNKNOWN IMPORTED)
        set_target_properties(PCAP::PCAP PROPERTIES
            IMPORTED_LOCATION         "${PCAP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PCAP_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(PCAP_INCLUDE_DIR PCAP_LIBRARY)
