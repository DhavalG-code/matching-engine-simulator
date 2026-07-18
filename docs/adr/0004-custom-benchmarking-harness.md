# Custom Benchmarking Harness

To ensure the C++ project remains 100% self-contained and compile-and-run ready on any platform without external dependencies (like Google Benchmark installation), a custom high-precision benchmark harness will be built directly into the file. It will use x86 CPU cycle counters (`__rdtsc()`) and standard `std::chrono::high_resolution_clock` to measure and output execution speed and throughput.
