add_executable(shmring_bench bench_ring_buffer.cpp)
target_link_libraries(shmring_bench PRIVATE
    shmring_headers benchmark::benchmark_main rt)
# Intentionally not registered with CTest; run manually.
