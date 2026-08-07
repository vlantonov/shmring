# Always build both binaries; link pcap conditionally via SHMRING_HAVE_PCAP macro.
add_executable(shmring_demo_producer producer.cpp)
target_link_libraries(shmring_demo_producer PRIVATE shmring_headers rt)

add_executable(shmring_demo_consumer consumer.cpp)
target_link_libraries(shmring_demo_consumer PRIVATE shmring_headers rt)

if(PCAP_FOUND)
    target_compile_definitions(shmring_demo_producer PRIVATE SHMRING_HAVE_PCAP)
    target_include_directories(shmring_demo_producer PRIVATE ${PCAP_INCLUDE_DIRS})
    target_link_libraries(shmring_demo_producer PRIVATE ${PCAP_LIBRARIES})
endif()
