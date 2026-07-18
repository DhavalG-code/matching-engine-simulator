# Fixed Capacity and Flat Memory Layout

To achieve ultra-low latency and eliminate runtime dynamic allocation jitter, the matching engine will use flat, pre-allocated memory structures with fixed capacity limits. Prices will be represented as fixed-point integers (`uint32_t`), and active orders will be managed in a contiguous pre-allocated array of `Order` structs with a maximum size of 100,000. Price levels for bids and asks will be tracked up to a fixed depth of 100 levels to ensure predictability and cache friendliness.
