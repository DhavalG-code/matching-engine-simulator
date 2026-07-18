/**
 * =================================================================================
 *   LOW-LATENCY ORDER BOOK & MATCHING ENGINE SIMULATOR (WITH C++ CPU OPTIMIZATIONS)
 * =================================================================================
 *
 * This file is an educational reference implementation demonstrating the high-performance
 * C++ design patterns and optimization strategies discussed in the paper:
 * "C++ Design Patterns for Low-Latency Applications Including High-Frequency Trading"
 *
 * Optimizations Implemented & Explained in Comments:
 * 1. Flat, Contiguous Memory Layout (No dynamic allocation on the hot path)
 * 2. Multi-Producer Single-Consumer (MPSC) Lock-Free Circular Ring Buffer (LMAX Disruptor Pattern)
 * 3. Cache Line Padding (Preventing False Sharing via alignas(64))
 * 4. AVX2 SIMD Vector Intrinsics (Parallel VWAP and cumulative volume stats)
 * 5. Branch Reduction & Validation via Bitwise Flags
 * 6. Slowpath Isolation via Non-Inlined Logging Functions ([[noinline]])
 * 7. Type Consistency (Float vs. Double implicit promotions)
 * 8. Signed vs. Unsigned Loop-Index Assembly Optimizations
 *
 * Compiling this file:
 *   g++ -O3 -std=c++17 -mavx2 matching_engine.cpp -o matching_engine.exe
 */

// Target Windows Vista/7 or higher to enable Condition Variable APIs in Windows SDK headers
#if defined(_WIN32)
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
#endif

#include <iostream>
#include <vector>
#include <atomic>
#include <queue>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iomanip>
#include <functional>
#include <memory>      // Header for std::unique_ptr and std::make_unique
#include <immintrin.h> // Header for x86 AVX2 SIMD Intrinsics

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <x86intrin.h> // Header for __rdtsc() CPU Cycle Counter
#endif

// =================================================================================
// MINGW THREAD FALLBACK FOR WINDOWS API
// Some older MinGW / GCC installations on Windows compile under the 'win32' threading
// model where <thread> and <mutex> are disabled. We implement a clean Win32-based
// wrapper to guarantee compiling and running under all Windows environments.
// =================================================================================
#if defined(_WIN32)
    #include <windows.h>
    #define USE_WIN32_THREAD_FALLBACK 1

    namespace std_fallback {
        class mutex {
        private:
            CRITICAL_SECTION cs;
        public:
            mutex() { InitializeCriticalSection(&cs); }
            ~mutex() { DeleteCriticalSection(&cs); }
            void lock() { EnterCriticalSection(&cs); }
            void unlock() { LeaveCriticalSection(&cs); }
            typedef LPVOID native_handle_type;
            native_handle_type native_handle() { return &cs; }
        };

        template <typename Mutex>
        class unique_lock {
        private:
            Mutex& mtx;
            bool is_locked;
        public:
            unique_lock(Mutex& m) : mtx(m), is_locked(true) { mtx.lock(); }
            ~unique_lock() { if (is_locked) mtx.unlock(); }
            void lock() { mtx.lock(); is_locked = true; }
            void unlock() { mtx.unlock(); is_locked = false; }
            Mutex* mutex() const noexcept { return &mtx; }
        };

        template <typename Mutex>
        class lock_guard {
        private:
            Mutex& mtx;
        public:
            lock_guard(Mutex& m) : mtx(m) { mtx.lock(); }
            ~lock_guard() { mtx.unlock(); }
        };

        class condition_variable {
        private:
            CONDITION_VARIABLE cv;
        public:
            condition_variable() { InitializeConditionVariable(&cv); }
            void notify_one() { WakeConditionVariable(&cv); }
            void notify_all() { WakeAllConditionVariable(&cv); }
            
            template <typename Lock>
            void wait(Lock& lock) {
                SleepConditionVariableCS(&cv, (PCRITICAL_SECTION)lock.mutex()->native_handle(), INFINITE);
            }

            template <typename Lock, typename Predicate>
            void wait(Lock& lock, Predicate pred) {
                while (!pred()) {
                    wait(lock);
                }
            }
        };

        class thread {
        private:
            HANDLE handle;

            static DWORD WINAPI ThreadProxy(LPVOID lpParam) {
                // Cast back to the heap-allocated function pointer, execute it,
                // and clean it up to prevent memory leaks. This heap allocation
                // makes thread moves (e.g. inside std::vector resize) completely safe.
                auto* func = static_cast<std::function<void()>*>(lpParam);
                (*func)();
                delete func;
                return 0;
            }
        public:
            thread() : handle(NULL) {}
            
            template <typename Callable, typename... Args>
            explicit thread(Callable&& f, Args&&... args) {
                auto* func_ptr = new std::function<void()>(
                    std::bind(std::forward<Callable>(f), std::forward<Args>(args)...)
                );
                handle = CreateThread(NULL, 0, ThreadProxy, func_ptr, 0, NULL);
            }

            ~thread() {
                if (handle) {
                    CloseHandle(handle);
                }
            }

            void join() {
                if (handle) {
                    WaitForSingleObject(handle, INFINITE);
                    CloseHandle(handle);
                    handle = NULL;
                }
            }

            thread(thread&& other) noexcept : handle(other.handle) {
                other.handle = NULL;
            }
            thread& operator=(thread&& other) noexcept {
                if (this != &other) {
                    if (handle) CloseHandle(handle);
                    handle = other.handle;
                    other.handle = NULL;
                }
                return *this;
            }
            
            thread(const thread&) = delete;
            thread& operator=(const thread&) = delete;
        };

        namespace this_thread {
            inline void yield() noexcept {
                SwitchToThread();
            }
        }
    }

    namespace thread_ns = std_fallback;
#else
    // POSIX Systems (Linux / macOS) support standard C++11 thread libraries out of the box
    #include <thread>
    #include <mutex>
    #include <condition_variable>
    #define USE_WIN32_THREAD_FALLBACK 0
    namespace thread_ns = std;
#endif

// =================================================================================
// 1. CONSTANTS & SYSTEM PARAMETERS
// =================================================================================
constexpr size_t MAX_DEPTH = 100;         // Max price levels in the book
constexpr size_t MAX_ORDERS = 100000;     // Pre-allocated active order pool limit
constexpr size_t RING_BUFFER_SIZE = 4096; // Circular buffer size (Must be a power of 2!)

// Cache Line Size on modern x86 processors is 64 bytes.
// Variables located on the same cache line and modified by different threads cause
// "False Sharing" (cache invalidation loops). We pad core sequences to 64 bytes.
constexpr size_t CACHE_LINE_SIZE = 64;

// =================================================================================
// 2. CORE DOMAIN DATA STRUCTURES (ADR-0001: Flat POD Layout)
// =================================================================================

/**
 * Order Status Validation Flags (Branchless Validation)
 * Instead of checking fields one-by-one with branching `if` statements, we calculate
 * a validation bitmask. This maps directly to a CPU instruction register flag check.
 */
enum ValidationFlags : uint8_t {
    VALID_ID       = 1 << 0,
    VALID_PRICE    = 1 << 1,
    VALID_QTY      = 1 << 2,
    VALID_SIDE     = 1 << 3,
    ALL_VALID      = 0x0F
};

/**
 * Plain Old Data (POD) representing a buy or sell limit order.
 * Using a flat POD layout avoids pointer chasing and keeps data contiguous in CPU caches.
 * C++ structure member alignment is optimized: largest members first to reduce padding bytes.
 */
struct alignas(32) Order {
    uint64_t orderId;        // 8 bytes
    uint32_t price;          // 4 bytes - Fixed point integer (e.g. Cents, $100.50 -> 10050)
    uint32_t quantity;       // 4 bytes
    uint32_t filledQuantity; // 4 bytes
    bool isBuy;              // 1 byte
    bool active;             // 1 byte
    uint8_t padding[10];     // 10 bytes padding to align structure cleanly to 32 bytes (cache line friendly)
};

/**
 * Trade execution report structure.
 */
struct Trade {
    uint64_t tradeId;
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    uint32_t price;
    uint32_t quantity;
    uint64_t cycleTimestamp; // TSC CPU Cycle count when the trade was executed
};

/**
 * Simple Price Level representation used for book depth calculations.
 */
struct LevelInfo {
    uint32_t price;
    uint32_t volume;
};

// =================================================================================
// 3. SLOWPATH ISOLATION (Non-Inlined Logging Functions)
// =================================================================================
// By telling the compiler NOT to inline logging/error paths, we keep the core loop
// code footprint tiny. The hot path stays inside the CPU L1 Instruction Cache (I-cache),
// reducing I-cache misses.
#if defined(_MSC_VER)
    #define NO_INLINE __declspec(noinline)
#else
    #define NO_INLINE __attribute__((noinline))
#endif

NO_INLINE void log_order_rejection(uint64_t orderId, uint8_t errorMask) {
    // Logging is extremely slow (incurs I/O bottlenecks). Keep it far away from the hot path.
    std::cout << "[REJECTED] Order " << orderId << " failed validation mask: " 
              << (int)errorMask << "\n";
}

NO_INLINE void log_trade(const Trade& trade) {
    // Trade reporting can be offloaded asynchronously in real production systems
}

// =================================================================================
// 4. FLAT-ARRAY ORDER BOOK (Zero Allocation on matching)
// =================================================================================
class OrderBook {
private:
    // Fixed-capacity arrays representing the Bid (buy) and Ask (sell) price levels.
    // Flat memory layout guarantees sequential scan speeds and avoids std::map tree traversal.
    LevelInfo bids[MAX_DEPTH];
    size_t bidCount = 0;

    LevelInfo asks[MAX_DEPTH];
    size_t askCount = 0;

    // Ring buffer of trades to store execution history without dynamic allocation
    Trade tradeHistory[MAX_ORDERS];
    size_t tradeCount = 0;

    uint64_t nextTradeId = 1;

    // Helper functions to insert/maintain sorted levels
    void insert_bid_level(uint32_t price, uint32_t qty) {
        // Find if price level exists
        for (size_t i = 0; i < bidCount; ++i) {
            if (bids[i].price == price) {
                bids[i].volume += qty;
                return;
            }
        }
        // If not, insert in sorted order (highest bid first)
        if (bidCount < MAX_DEPTH) {
            size_t insertIdx = bidCount;
            while (insertIdx > 0 && bids[insertIdx - 1].price < price) {
                bids[insertIdx] = bids[insertIdx - 1];
                --insertIdx;
            }
            bids[insertIdx] = {price, qty};
            ++bidCount;
        }
    }

    void insert_ask_level(uint32_t price, uint32_t qty) {
        // Find if price level exists
        for (size_t i = 0; i < askCount; ++i) {
            if (asks[i].price == price) {
                asks[i].volume += qty;
                return;
            }
        }
        // If not, insert in sorted order (lowest ask first)
        if (askCount < MAX_DEPTH) {
            size_t insertIdx = askCount;
            while (insertIdx > 0 && asks[insertIdx - 1].price > price) {
                asks[insertIdx] = asks[insertIdx - 1];
                --insertIdx;
            }
            asks[insertIdx] = {price, qty};
            ++askCount;
        }
    }

    void reduce_bid_volume(size_t index, uint32_t qty) {
        bids[index].volume -= qty;
        if (bids[index].volume == 0) {
            // Shift elements left
            for (size_t i = index; i < bidCount - 1; ++i) {
                bids[i] = bids[i + 1];
            }
            --bidCount;
        }
    }

    void reduce_ask_volume(size_t index, uint32_t qty) {
        asks[index].volume -= qty;
        if (asks[index].volume == 0) {
            // Shift elements left
            for (size_t i = index; i < askCount - 1; ++i) {
                asks[i] = asks[i + 1];
            }
            --askCount;
        }
    }

public:
    OrderBook() {
        std::memset(bids, 0, sizeof(bids));
        std::memset(asks, 0, sizeof(asks));
        std::memset(tradeHistory, 0, sizeof(tradeHistory));
    }

    /**
     * Hot Path matching execution function.
     * Takes an order, matches it against opposite book levels, and executes trades.
     */
    void process_order(Order& order) {
        // Validation using bitwise flags (Branch Reduction)
        // Eliminates nested conditional statements. Compiles to simple bitwise masking.
        uint8_t validationMask = 0;
        validationMask |= (order.orderId > 0) ? VALID_ID : 0;
        validationMask |= (order.price > 0) ? VALID_PRICE : 0;
        validationMask |= (order.quantity > 0) ? VALID_QTY : 0;
        validationMask |= (order.isBuy || !order.isBuy) ? VALID_SIDE : 0; // Trivial side check

        if (validationMask != ALL_VALID) {
            // Slowpath isolated: the error logging function is non-inlined
            log_order_rejection(order.orderId, validationMask);
            return;
        }

        uint32_t remainingQty = order.quantity;

        if (order.isBuy) {
            // Match against Asks (lowest ask first)
            while (askCount > 0 && remainingQty > 0 && asks[0].price <= order.price) {
                uint32_t matchQty = std::min(remainingQty, asks[0].volume);
                
                // Record Trade
                Trade& trade = tradeHistory[tradeCount++];
                trade = {
                    nextTradeId++,
                    order.orderId,
                    0, // In full engine, matches against specific active counterparty order ID
                    asks[0].price,
                    matchQty,
                    __rdtsc()
                };
                log_trade(trade);

                remainingQty -= matchQty;
                reduce_ask_volume(0, matchQty);
            }
            // If remaining quantity exists, add to bid levels
            if (remainingQty > 0) {
                insert_bid_level(order.price, remainingQty);
            }
        } else {
            // Match against Bids (highest bid first)
            while (bidCount > 0 && remainingQty > 0 && bids[0].price >= order.price) {
                uint32_t matchQty = std::min(remainingQty, bids[0].volume);

                // Record Trade
                Trade& trade = tradeHistory[tradeCount++];
                trade = {
                    nextTradeId++,
                    0,
                    order.orderId,
                    bids[0].price,
                    matchQty,
                    __rdtsc()
                };
                log_trade(trade);

                remainingQty -= matchQty;
                reduce_bid_volume(0, matchQty);
            }
            // If remaining quantity exists, add to ask levels
            if (remainingQty > 0) {
                insert_ask_level(order.price, remainingQty);
            }
        }
        order.filledQuantity = order.quantity - remainingQty;
        order.active = (remainingQty > 0);
    }

    // Level accessor for SIMD stats calculations
    const LevelInfo* get_bids() const { return bids; }
    size_t get_bid_count() const { return bidCount; }

    const LevelInfo* get_asks() const { return asks; }
    size_t get_ask_count() const { return askCount; }
};

// =================================================================================
// 5. C++ MICRO-OPTIMIZATIONS: AVX2 SIMD INTENT (ADR-0003)
// =================================================================================

/**
 * Baseline statistics calculator using sequential loop iteration.
 */
std::pair<float, uint32_t> calculate_stats_baseline(const LevelInfo* levels, size_t count) {
    size_t limit = std::min(count, size_t(32));
    if (limit == 0) return {0.0f, 0};

    float weightedSum = 0.0f;
    uint32_t totalVolume = 0;

    for (size_t i = 0; i < limit; ++i) {
        weightedSum += static_cast<float>(levels[i].price) * static_cast<float>(levels[i].volume);
        totalVolume += levels[i].volume;
    }

    float vwap = (totalVolume > 0) ? (weightedSum / static_cast<float>(totalVolume)) : 0.0f;
    return {vwap, totalVolume};
}

/**
 * Optimized statistics calculator using AVX2 SIMD vector instructions.
 * This loads and computes pricing statistics on 8 levels simultaneously.
 * Uses unaligned loads and stores to prevent General Protection Faults (crashes)
 * caused by potential compiler stack alignment bugs on older MinGW environments.
 */
std::pair<float, uint32_t> calculate_stats_simd(const LevelInfo* levels, size_t count) {
    size_t limit = std::min(count, size_t(32));
    if (limit == 0) return {0.0f, 0};

    // Make sure we process in multiples of 8. If less than 8, fallback to sequential logic.
    if (limit < 8) {
        return calculate_stats_baseline(levels, count);
    }

    // 256-bit SIMD registers to accumulate volumes and weighted sums
    __m256 v_volume_accum = _mm256_setzero_ps();
    __m256 v_weighted_accum = _mm256_setzero_ps();

    // Alignment and padding check: we scan levels in blocks of 8
    size_t i = 0;
    for (; i <= limit - 8; i += 8) {
        uint32_t prices[8];
        uint32_t volumes[8];

        for (int j = 0; j < 8; ++j) {
            prices[j] = levels[i + j].price;
            volumes[j] = levels[i + j].volume;
        }

        // Load 8 32-bit integers into YMM registers (using unaligned load for safety)
        __m256i v_prices_int = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(prices));
        __m256i v_volumes_int = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(volumes));

        // Convert the 32-bit integers into single-precision floating-point numbers (float)
        __m256 v_prices_float = _mm256_cvtepi32_ps(v_prices_int);
        __m256 v_volumes_float = _mm256_cvtepi32_ps(v_volumes_int);

        // Perform parallel multiplications and additions
        v_volume_accum = _mm256_add_ps(v_volume_accum, v_volumes_float);
        
        __m256 v_products = _mm256_mul_ps(v_prices_float, v_volumes_float);
        v_weighted_accum = _mm256_add_ps(v_weighted_accum, v_products);
    }

    // Accumulate any remainder elements sequentially
    float rem_weightedSum = 0.0f;
    uint32_t rem_totalVolume = 0;
    for (; i < limit; ++i) {
        rem_weightedSum += static_cast<float>(levels[i].price) * static_cast<float>(levels[i].volume);
        rem_totalVolume += levels[i].volume;
    }

    // Horizontal sum of the YMM vector lanes (using unaligned stores for safety)
    float final_volumes[8];
    float final_weighteds[8];
    _mm256_storeu_ps(final_volumes, v_volume_accum);
    _mm256_storeu_ps(final_weighteds, v_weighted_accum);

    float total_vol_float = rem_totalVolume;
    float total_weighted = rem_weightedSum;
    for (int j = 0; j < 8; ++j) {
        total_vol_float += final_volumes[j];
        total_weighted += final_weighteds[j];
    }

    float vwap = (total_vol_float > 0.0f) ? (total_weighted / total_vol_float) : 0.0f;
    return {vwap, static_cast<uint32_t>(total_vol_float)};
}

// =================================================================================
// 6. PIPELINED CONCURRENCY MECHANISMS (ADR-0002)
// =================================================================================

/**
 * Standard thread-safe queue baseline (Unoptimized Concurrency)
 * Relies on OS context-switching via mutex locks and thread awakening via condition variables.
 */
class MutexQueue {
private:
    std::queue<Order> queue;
    thread_ns::mutex mtx;
    thread_ns::condition_variable cv;
    bool done = false;

public:
    void push(const Order& order) {
        {
            thread_ns::lock_guard<thread_ns::mutex> lock(mtx);
            queue.push(order);
        }
        cv.notify_one();
    }

    bool pop(Order& order) {
        thread_ns::unique_lock<thread_ns::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !queue.empty() || done; });
        if (queue.empty() && done) return false;
        order = queue.front();
        queue.pop();
        return true;
    }

    void shutdown() {
        {
            thread_ns::lock_guard<thread_ns::mutex> lock(mtx);
            done = true;
        }
        cv.notify_all();
    }
};

/**
 * Lock-Free Ring Buffer (LMAX Disruptor Pattern - Multi-Producer Single-Consumer)
 * Supports multiple threads placing events simultaneously without mutex contention.
 * The consumer thread spins in user-space, avoiding the expensive CPU context switch
 * overhead of mutex wakes (which can exceed 3000ns per wake).
 */
class DisruptorRingBuffer {
private:
    struct RingEvent {
        // Atomic tag sequence acts as an execution publish barrier.
        // Uninitialized slot is marked with 0xFFFFFFFFFFFFFFFF.
        std::atomic<uint64_t> sequence{0xFFFFFFFFFFFFFFFFULL};
        Order order;
    };

    RingEvent buffer[RING_BUFFER_SIZE];

    // Core sequence counters aligned to separate cache lines (alignas(64))
    // to prevent False Sharing / CPU Cache Line Bouncing.
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> claimSequence{0}; 
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> consumerSequence{0};

public:
    DisruptorRingBuffer() {
        for (size_t i = 0; i < RING_BUFFER_SIZE; ++i) {
            buffer[i].sequence.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
        }
    }

    /**
     * Called by multiple producer threads concurrently (MPSC).
     * Uses atomic Compare-And-Swap (fetch_add) to claim a sequence slot,
     * waits for the slot to empty, writes, and publishes with release semantics.
     */
    void publish(const Order& order) {
        // Claim the next incremental sequence slot atomically
        uint64_t seq = claimSequence.fetch_add(1, std::memory_order_relaxed);

        size_t idx = seq & (RING_BUFFER_SIZE - 1); // Fast bitwise modulo (size is power of 2)

        // Wait strategy if ring buffer is full (slowdown/backpressure check)
        while (seq >= consumerSequence.load(std::memory_order_acquire) + RING_BUFFER_SIZE) {
            thread_ns::this_thread::yield(); // Yield CPU back to OS during severe backpressure
        }

        // Write the event data
        buffer[idx].order = order;

        // Publish barrier: make it visible to the consumer.
        // std::memory_order_release guarantees that the order data writes are complete
        // before the sequence change becomes visible to checking threads.
        buffer[idx].sequence.store(seq, std::memory_order_release);
    }

    /**
     * Called by a single matching engine consumer thread.
     * Uses a Busy-Spin Strategy (continually polling the ring buffer slot) for lowest latency.
     */
    bool consume(Order& order, uint64_t targetSeq) {
        size_t idx = targetSeq & (RING_BUFFER_SIZE - 1);

        // Busy-Spin Strategy: Spin until the slot's publish sequence matches targetSeq
        while (buffer[idx].sequence.load(std::memory_order_acquire) != targetSeq) {
            #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause(); // Emits instruction to optimize CPU pipeline during spin loops
            #endif
        }

        // Read event data
        order = buffer[idx].order;

        // Free the slot: mark sequence as uninitialized
        buffer[idx].sequence.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_release);

        // Advance consumer sequence tracker
        consumerSequence.store(targetSeq + 1, std::memory_order_release);
        return true;
    }
};

// =================================================================================
// 7. C++ MICRO-BENCHMARKS (ADR-0004: Self-Contained Benchmarks)
// =================================================================================

/**
 * Signed vs Unsigned loop performance comparison.
 * The paper highlights that compilers generate additional instructions for unsigned indexes
 * to check and handle defined overflow wrap-arounds correctly.
 */
NO_INLINE uint64_t run_unsigned_loop(unsigned iterations) {
    uint64_t result = 0;
    // Unsigned index: compiler must handle overflow checks if calculations occur inside
    for (unsigned i = 0; i < iterations; ++i) {
        result ^= (i * 33);
    }
    return result;
}

NO_INLINE uint64_t run_signed_loop(int iterations) {
    uint64_t result = 0;
    // Signed index: compiler assumes no overflow occurs (Undefined Behavior),
    // allowing it to optimize register allocations and unroll iterations cleanly.
    for (int i = 0; i < iterations; ++i) {
        result ^= (i * 33);
    }
    return result;
}

/**
 * Type Consistency (Float vs Double mixing) performance comparison.
 */
NO_INLINE float run_mixed_type_calc(float input, int iterations) {
    float val = input;
    for (int i = 0; i < iterations; ++i) {
        // Jitter / promotions: 1.23 is a double literal. val is promoted to double,
        // multiplied, and the result is demoted back to float to assign to val.
        val = val * 1.23; 
    }
    return val;
}

NO_INLINE float run_consistent_type_calc(float input, int iterations) {
    float val = input;
    for (int i = 0; i < iterations; ++i) {
        // Consistent: 1.23f is a float literal. Single instruction float multiply.
        val = val * 1.23f; 
    }
    return val;
}

// =================================================================================
// 8. SYSTEM BENCHMARK HARNESS & MAIN ORCHESTRATION
// =================================================================================
int main() {
    std::cout << "===============================================================\n";
    std::cout << "  C++ DESIGN PATTERNS FOR LOW-LATENCY APPLICATIONS SIMULATOR    \n";
    std::cout << "===============================================================\n\n";

    // Setup input order book data on the heap (avoids Stack Overflow)
    auto book = std::make_unique<OrderBook>();
    for (uint32_t i = 1; i <= 32; ++i) {
        Order buyOrder{i, 10000 + i * 10, 100 * i, 0, true, true};
        book->process_order(buyOrder);
    }

    // -----------------------------------------------------------------------------
    // BENCHMARK 1: AVX2 SIMD vs Baseline Loop Book Stats (VWAP Calculation)
    // -----------------------------------------------------------------------------
    std::cout << "[1/4] Running SIMD (AVX2) vs. Baseline Loop Stats Benchmark...\n" << std::flush;
    constexpr size_t STATS_RUNS = 100000;
    
    // Baseline timing
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t startCycles = __rdtsc();
    std::pair<float, uint32_t> baselineResult;
    for (size_t r = 0; r < STATS_RUNS; ++r) {
        baselineResult = calculate_stats_baseline(book->get_bids(), book->get_bid_count());
    }
    uint64_t endCycles = __rdtsc();
    auto end = std::chrono::high_resolution_clock::now();
    double baselineNs = std::chrono::duration<double, std::nano>(end - start).count() / STATS_RUNS;
    double baselineCyclesPerRun = static_cast<double>(endCycles - startCycles) / STATS_RUNS;

    // SIMD timing
    start = std::chrono::high_resolution_clock::now();
    startCycles = __rdtsc();
    std::pair<float, uint32_t> simdResult;
    for (size_t r = 0; r < STATS_RUNS; ++r) {
        simdResult = calculate_stats_simd(book->get_bids(), book->get_bid_count());
    }
    endCycles = __rdtsc();
    end = std::chrono::high_resolution_clock::now();
    double simdNs = std::chrono::duration<double, std::nano>(end - start).count() / STATS_RUNS;
    double simdCyclesPerRun = static_cast<double>(endCycles - startCycles) / STATS_RUNS;

    std::cout << "      * Baseline VWAP: " << baselineResult.first << " | Vol: " << baselineResult.second << "\n";
    std::cout << "      * SIMD VWAP:     " << simdResult.first     << " | Vol: " << simdResult.second     << "\n" << std::flush;
    if (std::abs(baselineResult.first - simdResult.first) >= 0.1f) {
        std::cout << "Result mismatch! Baseline: " << baselineResult.first << ", SIMD: " << simdResult.first << std::endl;
    } else {
        std::cout << "VWAP Check PASSED!" << std::endl;
    }

    // -----------------------------------------------------------------------------
    // BENCHMARK 2: Type Consistency & Mixed float promotions
    // -----------------------------------------------------------------------------
    std::cout << "[2/4] Running Type Consistency Benchmark (Float vs. Double Promotion)...\n" << std::flush;
    constexpr int TYPE_CALC_ITERATIONS = 10000;
    constexpr int TYPE_BENCH_RUNS = 10000;

    start = std::chrono::high_resolution_clock::now();
    startCycles = __rdtsc();
    float mixedSum = 0.0f;
    for (int r = 0; r < TYPE_BENCH_RUNS; ++r) {
        mixedSum += run_mixed_type_calc(1.0f + r, TYPE_CALC_ITERATIONS);
    }
    endCycles = __rdtsc();
    end = std::chrono::high_resolution_clock::now();
    double mixedNs = std::chrono::duration<double, std::nano>(end - start).count() / TYPE_BENCH_RUNS;
    double mixedCycles = static_cast<double>(endCycles - startCycles) / TYPE_BENCH_RUNS;

    start = std::chrono::high_resolution_clock::now();
    startCycles = __rdtsc();
    float consistentSum = 0.0f;
    for (int r = 0; r < TYPE_BENCH_RUNS; ++r) {
        consistentSum += run_consistent_type_calc(1.0f + r, TYPE_CALC_ITERATIONS);
    }
    endCycles = __rdtsc();
    end = std::chrono::high_resolution_clock::now();
    double consistentNs = std::chrono::duration<double, std::nano>(end - start).count() / TYPE_BENCH_RUNS;
    double consistentCycles = static_cast<double>(endCycles - startCycles) / TYPE_BENCH_RUNS;

    // Prevent optimizer from deleting sums
    (void)mixedSum;
    (void)consistentSum;

    // -----------------------------------------------------------------------------
    // BENCHMARK 3: Signed vs. Unsigned Loops
    // -----------------------------------------------------------------------------
    std::cout << "[3/4] Running Signed vs. Unsigned Loop-Index Benchmark...\n" << std::flush;
    constexpr int LOOP_ITERATIONS = 20000;
    constexpr int LOOP_BENCH_RUNS = 10000;

    start = std::chrono::high_resolution_clock::now();
    startCycles = __rdtsc();
    uint64_t unsignedSum = 0;
    for (int r = 0; r < LOOP_BENCH_RUNS; ++r) {
        unsignedSum += run_unsigned_loop(LOOP_ITERATIONS);
    }
    endCycles = __rdtsc();
    end = std::chrono::high_resolution_clock::now();
    double unsignedNs = std::chrono::duration<double, std::nano>(end - start).count() / LOOP_BENCH_RUNS;
    double unsignedCycles = static_cast<double>(endCycles - startCycles) / LOOP_BENCH_RUNS;

    start = std::chrono::high_resolution_clock::now();
    startCycles = __rdtsc();
    uint64_t signedSum = 0;
    for (int r = 0; r < LOOP_BENCH_RUNS; ++r) {
        signedSum += run_signed_loop(LOOP_ITERATIONS);
    }
    endCycles = __rdtsc();
    end = std::chrono::high_resolution_clock::now();
    double signedNs = std::chrono::duration<double, std::nano>(end - start).count() / LOOP_BENCH_RUNS;
    double signedCycles = static_cast<double>(endCycles - startCycles) / LOOP_BENCH_RUNS;

    // Prevent optimizer from deleting loops
    (void)unsignedSum;
    (void)signedSum;

    // -----------------------------------------------------------------------------
    // BENCHMARK 4: Mutex Queue vs. Disruptor Ring Buffer (HFT Concurrency Simulator)
    // -----------------------------------------------------------------------------
    std::cout << "[4/4] Running Lock-Free Disruptor vs. Mutex Queue Benchmark (HFT Pipeline)...\n" << std::flush;
    constexpr size_t TOTAL_MESSAGES = 100000;
    constexpr int PRODUCER_THREADS_COUNT = 4;
    constexpr size_t MESSAGES_PER_PRODUCER = TOTAL_MESSAGES / PRODUCER_THREADS_COUNT;

    // Heap allocation of queue systems and books to prevent stack overflow
    auto mutexQueue = std::make_unique<MutexQueue>();
    auto mutexBook = std::make_unique<OrderBook>();

    auto consumerFuncMutex = [&]() {
        Order order;
        while (mutexQueue->pop(order)) {
            mutexBook->process_order(order);
        }
    };

    thread_ns::thread mutexConsumerThread(consumerFuncMutex);
    std::vector<thread_ns::thread> mutexProducers;

    auto startMutexTime = std::chrono::high_resolution_clock::now();
    uint64_t startMutexCycles = __rdtsc();

    for (int t = 0; t < PRODUCER_THREADS_COUNT; ++t) {
        mutexProducers.emplace_back([&mutexQueue, t]() {
            for (size_t i = 0; i < MESSAGES_PER_PRODUCER; ++i) {
                Order order{1 + i + t * MESSAGES_PER_PRODUCER, 1000, 10, 0, true, true};
                mutexQueue->push(order);
            }
        });
    }

    for (auto& t : mutexProducers) {
        t.join();
    }
    mutexQueue->shutdown();
    mutexConsumerThread.join();

    uint64_t endMutexCycles = __rdtsc();
    auto endMutexTime = std::chrono::high_resolution_clock::now();
    double mutexNsTotal = std::chrono::duration<double, std::nano>(endMutexTime - startMutexTime).count();
    double mutexAvgNsPerOrder = mutexNsTotal / TOTAL_MESSAGES;
    double mutexAvgCyclesPerOrder = static_cast<double>(endMutexCycles - startMutexCycles) / TOTAL_MESSAGES;

    // B. Benchmark Disruptor-based Lock-Free Pipeline
    auto disruptorRing = std::make_unique<DisruptorRingBuffer>();
    auto disruptorBook = std::make_unique<OrderBook>();

    auto consumerFuncDisruptor = [&]() {
        Order order;
        for (size_t i = 0; i < TOTAL_MESSAGES; ++i) {
            disruptorRing->consume(order, i);
            disruptorBook->process_order(order);
        }
    };

    thread_ns::thread disruptorConsumerThread(consumerFuncDisruptor);
    std::vector<thread_ns::thread> disruptorProducers;

    auto startDisruptorTime = std::chrono::high_resolution_clock::now();
    uint64_t startDisruptorCycles = __rdtsc();

    for (int t = 0; t < PRODUCER_THREADS_COUNT; ++t) {
        disruptorProducers.emplace_back([&disruptorRing, t]() {
            for (size_t i = 0; i < MESSAGES_PER_PRODUCER; ++i) {
                Order order{1 + i + t * MESSAGES_PER_PRODUCER, 1000, 10, 0, true, true};
                disruptorRing->publish(order);
            }
        });
    }

    for (auto& t : disruptorProducers) {
        t.join();
    }
    disruptorConsumerThread.join();

    uint64_t endDisruptorCycles = __rdtsc();
    auto endDisruptorTime = std::chrono::high_resolution_clock::now();
    double disruptorNsTotal = std::chrono::duration<double, std::nano>(endDisruptorTime - startDisruptorTime).count();
    double disruptorAvgNsPerOrder = disruptorNsTotal / TOTAL_MESSAGES;
    double disruptorAvgCyclesPerOrder = static_cast<double>(endDisruptorCycles - startDisruptorCycles) / TOTAL_MESSAGES;

    // -----------------------------------------------------------------------------
    // PRINT BENCHMARK COMPARISON RESULTS
    // -----------------------------------------------------------------------------
    std::cout << "\n";
    std::cout << "=========================================================================\n";
    std::cout << "                       BENCHMARK PERFORMANCE RESULTS                     \n";
    std::cout << "=========================================================================\n";
    std::cout << std::left << std::setw(30) << "Benchmark Scenario" 
              << std::right << std::setw(20) << "Avg Latency (ns)" 
              << std::right << std::setw(20) << "Avg CPU Cycles" << "\n";
    std::cout << "-------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(30) << "VWAP Stats: Baseline Loop" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << baselineNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << baselineCyclesPerRun << "\n";

    std::cout << std::left << std::setw(30) << "VWAP Stats: AVX2 SIMD" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << simdNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << simdCyclesPerRun << "\n";

    std::cout << "-------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(30) << "Data Types: Mixed Promotion" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << mixedNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << mixedCycles << "\n";

    std::cout << std::left << std::setw(30) << "Data Types: Consistent Float" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << consistentNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << consistentCycles << "\n";

    std::cout << "-------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(30) << "Loop Type: Unsigned Loop Index" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << unsignedNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << unsignedCycles << "\n";

    std::cout << std::left << std::setw(30) << "Loop Type: Signed Loop Index" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << signedNs 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << signedCycles << "\n";

    std::cout << "-------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(30) << "Pipeline: Mutex Queue" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << mutexAvgNsPerOrder 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << mutexAvgCyclesPerOrder << "\n";

    std::cout << std::left << std::setw(30) << "Pipeline: Lock-Free Disruptor" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << disruptorAvgNsPerOrder 
              << std::right << std::setw(20) << std::fixed << std::setprecision(0) << disruptorAvgCyclesPerOrder << "\n";

    std::cout << "=========================================================================\n\n";

    // Prevent compiler from optimizing away the micro-benchmarks by storing their results in a volatile variable
    volatile double dummy = (double)mixedSum + (double)consistentSum + (double)unsignedSum + (double)signedSum;
    (void)dummy;

    return 0;
}
