# Concurrency and Disruptor Pipeline

To test multi-threaded event dispatching at scale, the simulator will support a Multi-Producer Single-Consumer (MPSC) architecture:
1. Multiple Order Producer threads will submit orders concurrently.
2. The optimized pipeline will implement a lock-free Ring Buffer using atomic Compare-And-Swap (CAS) instructions to allow producers to safely claim sequence slots in a race.
3. The Matching Engine (consumer thread) will process orders by busy-spinning on a sequence barrier to read and match orders without lock contention or thread context switches.
