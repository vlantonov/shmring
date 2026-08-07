add_executable(shmring_tests test_ring_buffer.cpp)
target_link_libraries(shmring_tests PRIVATE
    shmring_headers Catch2::Catch2WithMain rt)

include(Catch)
catch_discover_tests(shmring_tests)
