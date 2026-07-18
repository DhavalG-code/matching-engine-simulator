# Matching Engine Context

A simulator for a low-latency limit order book and matching engine that handles high-throughput incoming orders and executes trades.

## Language

**Order**:
A request by a market participant to buy or sell a specified quantity of a security at a price.
_Avoid_: Purchase, transaction

**Price-Time Priority**:
The matching priority rule where orders at the best price are filled first, and among orders at the same price, the one that arrived earliest is filled first.
_Avoid_: First-come first-served

**Trade**:
An execution resulting from matching a buy order and a sell order.
_Avoid_: Transaction, deal, fill

**Order Book**:
A collection of active, unmatched buy and sell orders, organized by price levels.
_Avoid_: Queue, ledger

**Order Producer**:
A component or thread that submits incoming orders to the matching engine.
_Avoid_: Client, sender

**Matching Engine**:
The core processing component (Order Consumer) that matches incoming buy and sell orders.
_Avoid_: Matcher, processor
