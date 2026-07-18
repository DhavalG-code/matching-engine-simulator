# CPU-Level Micro-Optimizations

To demonstrate the micro-optimization strategies from the paper, the simulator will implement:
1. AVX2 SIMD Intrinsics to calculate Order Book statistics (Volume-Weighted Average Price and cumulative volume at depth) in parallel, bypassing sequential loop overhead.
2. Strict Type Consistency (float/double) to avoid implicit promotions and demotions.
3. Branch Reduction and Slowpath Isolation via bitwise flag error checks and `[[noinline]]` helper functions for order rejection and error logs.
