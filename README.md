# Sketches — A DuckDB Community Extension

## Table of Contents

- [Overview](#overview)
- [What is DuckDB](#what-is-duckdb)
- [What are DuckDB Community Extensions](#what-are-duckdb-community-extensions)
- [About This Extension](#about-this-extension)
- [The Paper](#the-paper)
- [The Four Sketches](#the-four-sketches)
- [Getting Started](#getting-started)
- [Building the Extension](#building-the-extension)
- [Loading the Extension](#loading-the-extension)
- [SQL Function Reference](#sql-function-reference)
- [Usage Examples](#usage-examples)
- [File Structure](#file-structure)
- [File Reference](#file-reference)
- [Performance and Accuracy](#performance-and-accuracy)
- [References](#references)

---

## Overview

**Sketches** is a DuckDB community extension that implements four probabilistic
data structures for approximate frequency estimation over data streams, as
described in the paper *Single Update Sketch with Variable Counter Structure*
(VLDB 2023). It provides SQL aggregate and scalar functions for building,
querying, updating, and merging frequency sketches directly inside DuckDB
queries, with no external dependencies.

---

## What is DuckDB

DuckDB is an in-process analytical SQL database management system designed
for fast analytical queries on large datasets. Unlike traditional client-server
databases, DuckDB runs entirely inside the host process — there is no separate
server to start or connect to. It is optimized for online analytical processing
(OLAP) workloads, supports standard SQL, and can read and write common file
formats such as CSV, Parquet, and JSON directly.

DuckDB is open source and available for Python, R, Java, Node.js, and as a
standalone command-line shell. Its columnar vectorized execution engine allows
it to process large datasets efficiently on a single machine.

> Official DuckDB website: https://duckdb.org
> DuckDB GitHub repository: https://github.com/duckdb/duckdb
> DuckDB documentation: https://duckdb.org/docs

---

## What are DuckDB Community Extensions

DuckDB has a flexible extension mechanism that allows developers to add new
functions, types, file format readers, and other capabilities without modifying
the DuckDB core. Extensions are compiled as shared libraries and loaded at
runtime with a single SQL command.

Extensions fall into two categories:

**Core extensions** are maintained by the DuckDB team and distributed with
DuckDB itself. Examples include the `parquet`, `json`, and `httpfs` extensions.

**Community extensions** are maintained by third-party developers and
distributed through the DuckDB Community Extension Repository. Any developer
can publish a community extension by opening a pull request to the community
repository with a descriptor file pointing to their source code. The community
CI system then builds the extension for all supported platforms (Linux, macOS,
Windows, WebAssembly) and makes it available to DuckDB users via:
```sql
INSTALL extension_name FROM community;
LOAD extension_name;
```

The official DuckDB extension template provides a batteries-included starting
point for building community extensions, including a CMake build system, a
vcpkg dependency manager integration, a SQL-based testing framework, and a
GitHub Actions CI/CD pipeline.

> Community extension repository: https://github.com/duckdb/community-extensions
> Extension template: https://github.com/duckdb/extension-template
> Community extension development guide: https://duckdb.org/community_extensions/development
> Extension documentation: https://duckdb.org/docs/extensions/overview

---

## About This Extension

This extension implements four sketch data structures for per-flow size
estimation, which is the problem of estimating how many times each distinct
item has appeared in a data stream. The sketches use sub-linear memory — they
do not store every item individually — and trade a small, bounded amount of
accuracy for dramatic reductions in memory usage and query time.

The primary motivation is high-speed network traffic measurement, where a
router or switch needs to estimate the number of packets in each network flow
(identified by source IP, destination IP, or a five-tuple) without storing the
full packet log. The same techniques apply to any domain where exact counting
is too expensive: web analytics, database query optimization, streaming
telemetry, and fraud detection.

This extension exposes the sketches as standard DuckDB aggregate functions.
You build a sketch by running a `GROUP BY` query, store the result as a `BLOB`
column, and query it later using scalar functions — all inside SQL with no
application code required.

---

## The Paper

This extension implements the four baseline sketch structures evaluated in:

> Dimitrios Melissourgos, Haibo Wang, Shigang Chen, Chaoyi Ma, and Shiping Chen.
> **Single Update Sketch with Variable Counter Structure.**
> *Proceedings of the VLDB Endowment*, Vol. 16, No. 13, pp. 4296–4309, 2023.
> https://doi.org/10.14778/3625054.3625065
> Full version: https://github.com/haiporwang/ssvs/blob/main/main.pdf
> Reference implementation: https://github.com/DimitrisMel/SSVS

The paper proposes a new sketch called SSVS (Single Update Sketch with Variable
Counter Structure) and benchmarks it against four existing baseline sketches:
CM, CU, CS, and RCS. This extension implements those four baselines so they
can be compared directly inside DuckDB on real datasets.

The paper classifies sketches into two groups:

- **Multi-update sketches** update multiple counters per item, giving higher
  accuracy at the cost of more memory writes per packet. CM, CU, and CS are
  multi-update sketches.

- **Single-update sketches** update only one counter per item, giving much
  lower processing overhead at the cost of higher estimation error. RCS is a
  single-update sketch.

---

## The Four Sketches

### CM — Count-Min Sketch

The Count-Min Sketch was introduced by Cormode and Muthukrishnan in 2005.
It maintains a two-dimensional array of counters with `depth` rows and `width`
columns. Each row has an independent hash function.

**Insert:** For each incoming item, hash it with each row's hash function to
get a column index, then increment that counter in every row. This is `depth`
counter writes per item.

**Query:** Hash the query item with each row's hash function and return the
minimum counter value across all rows. The minimum is the best estimate because
hash collisions can only cause overcounting, never undercounting.

**Accuracy guarantee:** estimate <= true_count + ε * N   with probability 1 - δ
where ε = e / width   and   δ = (1/e)^depth

> Original paper: https://dsf.berkeley.edu/cs286/papers/countmin-latin2004.pdf
> Wikipedia: https://en.wikipedia.org/wiki/Count%E2%80%93min_sketch

---

### CU — Counter Update Sketch

The Counter Update Sketch was introduced by Estan and Varghese and is sometimes
called Count-Min with Conservative Update. It uses the same two-dimensional
array structure as CM but applies a more careful update rule.

**Insert:** For each incoming item, first read all `depth` counters (one per
row) to find the current minimum value. Then only increment the counter(s) that
equal that minimum. Counters already above the minimum have been inflated by
collisions from other flows and should not be incremented further.

**Query:** Identical to CM — return the minimum counter value across all rows.

**Why it helps:** By not incrementing already-inflated counters, CU reduces
overcounting compared to CM with the same memory. The cost is two passes per
insert instead of one.

> Reference: Estan and Varghese, "New Directions in Traffic Measurement and
> Accounting", ACM SIGCOMM 2002.
> https://dl.acm.org/doi/10.1145/633025.633056

---

### CS — Count Sketch

Count Sketch was introduced by Charikar, Chen, and Farach-Colton in 2002. It
uses signed counters and two independent hash functions per row.

**Insert:** For each incoming item and each row, compute a column hash (which
counter to update) and a sign hash (+1 or -1). Increment the counter by +1 or
-1 accordingly. This is `depth` writes per item like CM, but the signed updates
allow noise from different flows to cancel out in expectation.

**Query:** For each row, compute `counter * sign` to undo the sign encoding.
Return the median of these values across all rows. Median is used instead of
minimum because the signed noise is symmetric — some rows will overestimate and
some will underestimate, and the median is a more robust central estimate.

**Key difference from CM:** Count Sketch can undercount as well as overcount.
Its estimates are unbiased (expected error is zero), whereas CM always
overcounts.

**Accuracy guarantee:** |estimate - true_count| <= ε * ||f||_2   with probability 1 - δ
where ε = sqrt(3/width)  and  δ = (1/e)^depth / 2

> Original paper: https://link.springer.com/chapter/10.1007/3-540-45465-9_33
> Wikipedia: https://en.wikipedia.org/wiki/Count_sketch

---

### RCS — Randomized Counter Sharing

Randomized Counter Sharing was introduced by Hua, Zhao, Li, and Xu. It is the
only single-update sketch in this extension — it performs exactly one counter
write per incoming item regardless of `l`.

**Structure:** A single flat array of `width` shared counters (not a 2D array).
Each flow is mapped to `l` of these counters via `l` independent hash functions.
All flows share the same counter array.

**Insert:** For each incoming item, randomly select ONE of the flow's `l`
counters and increment it by 1. The random selection uses the total number of
items seen so far as a per-item seed, ensuring different items of the same flow
select different counters uniformly over time.

**Query:** Sum the flow's `l` counters. Subtract the estimated noise from other
flows that share those counters: noise = l * (sum of all counters / total counters)

The noise estimate is valid because the single-update random selection ensures
that each counter accumulates noise uniformly from all flows. Clamp the result
to zero since flow size cannot be negative.

**The key tradeoff:** RCS achieves much lower processing overhead than CM/CU/CS
because it only writes one counter per item. However, it requires a large `l`
(the paper uses `l=50`) for the noise distribution to be uniform enough for
accurate estimation. Larger `l` means more noise in each flow's estimate but
better randomization of that noise.

> Original paper: Hua et al., "Brick: A Novel Exact Active Statistics Counter
> Architecture", IEEE INFOCOM 2012.
> https://ieeexplore.ieee.org/document/6195598
> Full RCS reference: https://dl.acm.org/doi/10.1145/1851182.1851235

---

## Getting Started

### Prerequisites

| Tool | Minimum version | Install |
|------|----------------|---------|
| Git | Any recent | Pre-installed on macOS/Linux |
| CMake | 3.21+ | `brew install cmake` or `apt install cmake` |
| C++ compiler (C++17) | Clang 10+ or GCC 10+ | `xcode-select --install` (macOS) or `apt install build-essential` (Linux) |

On macOS you do not need the full Xcode IDE — the Command Line Tools are
sufficient.

> CMake download: https://cmake.org/download/
> Homebrew (macOS package manager): https://brew.sh

---

## Building the Extension

### Step 1: Clone the repository
```bash
git clone --recurse-submodules https://github.com/<your-username>/sketches.git
cd sketches
```

The `--recurse-submodules` flag is required. It pulls the DuckDB source code
and the DuckDB CI tools as git submodules. Without it the build will fail with
missing header errors.

If you already cloned without the flag, run:
```bash
git submodule update --init --recursive
```

### Step 2: Pin the DuckDB submodule to a stable release

The DuckDB `main` branch changes its extension API frequently. Pin to a known
stable release to avoid build failures:
```bash
cd duckdb
git fetch --all --tags
git checkout v1.2.0
cd ..
git add duckdb
```

### Step 3: Build
```bash
make
```

The first build takes 5–10 minutes because it compiles DuckDB from source.
Subsequent builds are fast since DuckDB is cached. A successful build ends with:
[100%] Linking CXX shared library sketches.duckdb_extension
[100%] Built target sketches

The compiled extension is at: build/release/extension/sketches/sketches.duckdb_extension

### Step 4: Run the tests
```bash
make test
```

Or open an interactive DuckDB shell with the extension already loaded:
```bash
./build/release/duckdb
```

---

## Loading the Extension

From within the DuckDB shell or any DuckDB client:
```sql
LOAD './build/release/extension/sketches/sketches.duckdb_extension';
```

Once loaded, all sixteen functions are available in the current session.

---

## SQL Function Reference

All sketches are stored as `BLOB` columns. The aggregate functions build and
serialize the sketch. The scalar functions operate on stored BLOBs.

### CM — Count-Min Sketch

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `countmin_agg` | `col VARCHAR [, depth INT [, width INT]]` | `BLOB` | Build a CM sketch over a column |
| `countmin_query` | `sketch BLOB, value VARCHAR` | `BIGINT` | Estimated count for a value |
| `countmin_update` | `sketch BLOB, value VARCHAR` | `BLOB` | Add one value to an existing sketch |
| `countmin_merge` | `sketch1 BLOB, sketch2 BLOB` | `BLOB` | Merge two CM sketches |

**Defaults:** `depth=5`, `width=2048`

**Accuracy:** Never undercounts. Estimate is always `>= true_count`.

---

### CU — Counter Update Sketch

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `cu_agg` | `col VARCHAR [, depth INT [, width INT]]` | `BLOB` | Build a CU sketch over a column |
| `cu_query` | `sketch BLOB, value VARCHAR` | `BIGINT` | Estimated count for a value |
| `cu_update` | `sketch BLOB, value VARCHAR` | `BLOB` | Add one value to an existing sketch |
| `cu_merge` | `sketch1 BLOB, sketch2 BLOB` | `BLOB` | Merge two CU sketches |

**Defaults:** `depth=5`, `width=2048`

**Accuracy:** Never undercounts. Less overcounting than CM for the same memory.

---

### CS — Count Sketch

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `countsketch_agg` | `col VARCHAR [, depth INT [, width INT]]` | `BLOB` | Build a CS sketch over a column |
| `countsketch_query` | `sketch BLOB, value VARCHAR` | `BIGINT` | Estimated count for a value |
| `countsketch_update` | `sketch BLOB, value VARCHAR` | `BLOB` | Add one value to an existing sketch |
| `countsketch_merge` | `sketch1 BLOB, sketch2 BLOB` | `BLOB` | Merge two CS sketches |

**Defaults:** `depth=5`, `width=2048`

**Accuracy:** Unbiased — can undercount or overcount. Better for low-frequency
items than CM/CU.

---

### RCS — Randomized Counter Sharing

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `rcs_agg` | `col VARCHAR [, l INT [, width INT]]` | `BLOB` | Build an RCS sketch over a column |
| `rcs_query` | `sketch BLOB, value VARCHAR` | `BIGINT` | Estimated count for a value |
| `rcs_update` | `sketch BLOB, value VARCHAR` | `BLOB` | Add one value to an existing sketch |
| `rcs_merge` | `sketch1 BLOB, sketch2 BLOB` | `BLOB` | Merge two RCS sketches |

**Defaults:** `l=50`, `width=2048`

**Accuracy:** Higher error than multi-update sketches for the same memory.
Single-update — much lower processing overhead.

---

## Usage Examples

### Basic frequency estimation
```sql
LOAD './build/release/extension/sketches/sketches.duckdb_extension';

-- Build a Count-Min sketch over a column
CREATE TABLE my_sketch AS
    SELECT countmin_agg(value) AS sketch
    FROM my_table;

-- Query estimated count for a specific value
SELECT countmin_query(sketch, 'some_value') AS estimated_count
FROM my_sketch;
```

---

### Network flow frequency estimation

This is the primary use case from the paper — estimating how often each
source/destination IP pair (flow) appears in a packet log.
```sql
-- Load raw packet log (tab-separated: src_ip, dst_ip)
CREATE TABLE ip_log AS
    SELECT column0 AS src, column1 AS dst
    FROM read_csv('packets.txt', sep='\t', header=false);

-- Build a Count-Min sketch of flows
CREATE TABLE flow_sketch AS
    SELECT countmin_agg(src || '->' || dst) AS sketch
    FROM ip_log;

-- Estimate how many packets a specific flow sent
SELECT countmin_query(sketch, '192.168.1.1->10.0.0.1') AS estimated_packets
FROM flow_sketch;

-- Query multiple flows at once
SELECT
    flow,
    countmin_query(sketch, flow) AS estimated_packets
FROM flow_sketch,
     (VALUES ('192.168.1.1->10.0.0.1'), ('172.16.0.5->8.8.8.8')) t(flow);
```

---

### Comparing all four sketches
```sql
-- Build all four sketches from the same data
CREATE TABLE sketch_cm  AS SELECT countmin_agg(src || '->' || dst)     AS sketch FROM ip_log;
CREATE TABLE sketch_cu  AS SELECT cu_agg(src || '->' || dst)           AS sketch FROM ip_log;
CREATE TABLE sketch_cs  AS SELECT countsketch_agg(src || '->' || dst)  AS sketch FROM ip_log;
CREATE TABLE sketch_rcs AS SELECT rcs_agg(src || '->' || dst)          AS sketch FROM ip_log;

-- Get exact counts for comparison
CREATE TABLE exact_counts AS
    SELECT src || '->' || dst AS flow, count(*) AS exact_count
    FROM ip_log GROUP BY flow;

-- Compare all four estimates against exact counts
SELECT
    e.flow,
    e.exact_count,
    countmin_query(cm.sketch,     e.flow) AS cm_estimate,
    cu_query(cu.sketch,           e.flow) AS cu_estimate,
    countsketch_query(cs.sketch,  e.flow) AS cs_estimate,
    rcs_query(rcs.sketch,         e.flow) AS rcs_estimate
FROM exact_counts e
CROSS JOIN sketch_cm  cm
CROSS JOIN sketch_cu  cu
CROSS JOIN sketch_cs  cs
CROSS JOIN sketch_rcs rcs
ORDER BY exact_count DESC
LIMIT 20;
```

---

### Accuracy measurement
```sql
-- Measure overcount for each sketch relative to exact counts
SELECT
    e.flow,
    e.exact_count,
    countmin_query(cm.sketch, e.flow) - e.exact_count AS cm_overcount,
    cu_query(cu.sketch,       e.flow) - e.exact_count AS cu_overcount,
    countsketch_query(cs.sketch, e.flow) - e.exact_count AS cs_error,
    rcs_query(rcs.sketch,     e.flow) - e.exact_count AS rcs_error
FROM exact_counts e
CROSS JOIN sketch_cm cm
CROSS JOIN sketch_cu cu
CROSS JOIN sketch_cs cs
CROSS JOIN sketch_rcs rcs
ORDER BY exact_count DESC;

-- Summary statistics
SELECT
    avg(abs(countmin_query(cm.sketch, e.flow)    - e.exact_count)) AS cm_avg_abs_error,
    avg(abs(cu_query(cu.sketch, e.flow)          - e.exact_count)) AS cu_avg_abs_error,
    avg(abs(countsketch_query(cs.sketch, e.flow) - e.exact_count)) AS cs_avg_abs_error,
    avg(abs(rcs_query(rcs.sketch, e.flow)        - e.exact_count)) AS rcs_avg_abs_error
FROM exact_counts e
CROSS JOIN sketch_cm  cm
CROSS JOIN sketch_cu  cu
CROSS JOIN sketch_cs  cs
CROSS JOIN sketch_rcs rcs;
```

---

### Timing queries with EXPLAIN ANALYZE
```sql
-- Time a single sketch query (most accurate timing method in DuckDB)
EXPLAIN ANALYZE
SELECT countmin_query(sketch, '192.168.1.1->10.0.0.1')
FROM sketch_cm;

-- Time a brute-force exact count for comparison
EXPLAIN ANALYZE
SELECT count(*) FROM ip_log
WHERE src || '->' || dst = '192.168.1.1->10.0.0.1';

-- Time 10,000 sketch queries at once
CREATE TABLE test_flows AS
    SELECT DISTINCT src || '->' || dst AS flow FROM ip_log LIMIT 10000;

EXPLAIN ANALYZE
SELECT countmin_query(sketch, flow)
FROM sketch_cm, test_flows;
```

---

### Streaming / continuous updates
```sql
-- Build an initial sketch
CREATE TABLE live_sketch AS
    SELECT countmin_agg(src || '->' || dst) AS sketch FROM ip_log;

-- Add a new flow packet without rebuilding the sketch
UPDATE live_sketch
SET sketch = countmin_update(sketch, '1.2.3.4->5.6.7.8');

-- Query the updated sketch
SELECT countmin_query(sketch, '1.2.3.4->5.6.7.8') FROM live_sketch;
```

---

### Merging sketches from different time windows
```sql
-- Build sketches on two separate time windows
CREATE TABLE sketch_window1 AS
    SELECT countmin_agg(src || '->' || dst) AS sketch
    FROM ip_log WHERE ts < '2024-01-01';

CREATE TABLE sketch_window2 AS
    SELECT countmin_agg(src || '->' || dst) AS sketch
    FROM ip_log WHERE ts >= '2024-01-01';

-- Merge into a combined sketch and query it
SELECT countmin_query(
    countmin_merge(w1.sketch, w2.sketch),
    '192.168.1.1->10.0.0.1'
) AS combined_estimate
FROM sketch_window1 w1, sketch_window2 w2;
```

---

### Custom sketch dimensions
```sql
-- More depth and width = more accurate but uses more memory
SELECT countmin_agg(flow, 7, 4096) AS sketch FROM flows;

-- RCS with a different l value
-- Higher l = better noise distribution but higher error per query
SELECT rcs_agg(flow, 100, 4096) AS sketch FROM flows;
```

---

## File Structure```
sketches/
    CMakeLists.txt
    extension_config.cmake
    Makefile
    duckdb/                        (git submodule, pinned v1.2.0)
    extension-ci-tools/            (git submodule)
    src/
        sketches_extension.cpp
        sketches_aggregate.cpp
        include/
            sketches_extension.hpp
            sketches_aggregate.hpp
    test/
        sql/
            sketches.test
```

---

## File Reference

### `src/include/sketches_extension.hpp`

Declares the `SketchesExtension` class that DuckDB calls when the extension
is loaded. Contains three methods:

- `Load(DuckDB &db)` — called once on load, registers all functions
- `Name()` — returns the string `"sketches"` used in the catalog
- `Version()` — returns the version string shown in `duckdb_extensions()`

Also contains a `using` declaration that exposes `SketchesExtension` in the
global namespace so DuckDB's auto-generated loader code can find it.

---

### `src/sketches_extension.cpp`

Implements `SketchesExtension::Load()` and the two C entry points that DuckDB
calls after loading the shared library:

- `sketches_init(DatabaseInstance &db)` — the main entry point called by DuckDB
- `sketches_version()` — returns the DuckDB version for ABI compatibility checking

Inside `Load()`, each function is registered in three steps:
1. Create a `ScalarFunctionSet` or `AggregateFunctionSet` with the SQL name
2. Add overloads for each combination of optional parameters
3. Call `ExtensionUtil::RegisterFunction()` to add it to DuckDB's catalog

Aggregate functions are registered with up to three overloads (1-arg, 2-arg,
3-arg) so that depth/width/l parameters are truly optional in SQL.

---

### `src/include/sketches_aggregate.hpp`

The largest file. Contains all four sketch data structures and their DuckDB
integration types. For each sketch there are four types:

**The sketch struct** (`CountMinSketch`, `CountSketch`, `RCSSketch`):
- Holds the counter array and hash seeds
- Implements `Insert()`, `Query()`, `Merge()`, `Serialize()`, `Deserialize()`
- Hash functions use multiply-add hashing modulo the Mersenne prime 2^31 - 1
  for fast, well-distributed values

**The state struct** (`CountMinState`, `CountSketchState`, `RCSState`):
- Holds a raw pointer to the heap-allocated sketch
- DuckDB allocates state as raw bytes with no constructor — the pointer must
  be explicitly set to `nullptr` in `Initialize()`

**The bind data struct** (`CountMinBindData`, `CountSketchBindData`, `RCSBindData`):
- Carries user-supplied parameters (depth, width, l) from query planning time
  through to every worker thread
- Must implement `Copy()` and `Equals()` for DuckDB's parallel execution

**The operation struct** (`CountMinOperation`, `CountSketchOperation`, `RCSOperation`):
- Contains the static template methods that DuckDB's aggregate machinery calls:
  - `Initialize()` — zero the state pointer
  - `Operation()` — process one row
  - `ConstantOperation()` — process a batch of identical rows (optimisation)
  - `Combine()` — merge two parallel states (called during parallel execution)
  - `Finalize()` — produce the final BLOB result
  - `Destroy()` — free heap memory (CRITICAL — without this every query leaks)
  - `IgnoreNull()` — returns true so NULL rows are skipped automatically

CM and CU share one struct (`CountMinSketch`) controlled by a `conservative`
flag serialized into the BLOB. This means `cu_query`, `cu_update`, and
`cu_merge` can reuse the CM scalar functions since the mode is stored in the
BLOB itself.

Also declares all factory function signatures at the bottom, implemented in
`sketches_aggregate.cpp`.

---

### `src/sketches_aggregate.cpp`

Implements all factory functions declared in the header. For each sketch type:

**Bind function** (`CountMinBind`, `CountMinConservativeBind`, `CountSketchBind`,
`RCSBind`):
- Called once at query planning time before any rows are processed
- Reads optional constant arguments using `ExpressionExecutor::EvaluateScalar()`
- Validates argument ranges and throws `BinderException` with a clear message
  if out of range
- Returns a `BindData` object that DuckDB attaches to every worker state

**Aggregate factory** (`GetCountMinAggregateFunction()` etc.):
- Calls `AggregateFunction::UnaryAggregate<STATE, INPUT, RESULT, OP>()` to
  wire up the DuckDB aggregate pipeline
- Attaches the bind function via `agg.bind`
- Attaches the destructor via `agg.destructor = AggregateFunction::StateDestroy<...>`
  so DuckDB calls `Destroy()` and frees heap memory

**Scalar functions** (`CountMinQueryFunction()`, `CountMinUpdateFunction()`,
`CountMinMergeFunction()` etc.):
- Use `UnifiedVectorFormat` to handle flat, constant, and dictionary vectors
  without special-casing
- Always go through `sel->get_index(i)` to map logical to physical row indices
- Check validity masks and propagate NULLs from either input to the output
- Deserialize the BLOB, run the operation, re-serialize if needed, and write
  the result into a flat output vector using
  `StringVector::AddStringOrBlob()` to copy bytes into DuckDB's string heap

---

## Performance and Accuracy

### Update cost (per item)

| Sketch | Counter writes per item | Memory reads per item |
|--------|------------------------|----------------------|
| CM | `depth` | 0 (write only) |
| CU | `depth` | `depth` (read min first) |
| CS | `depth` | 0 (write only) |
| RCS | 1 | 0 (single update) |

RCS has the lowest update cost by design — this is its primary advantage. The
paper reports RCS is roughly 10x faster than CU-SC (the most accurate
multi-update sketch) in per-packet processing time on the CAIDA dataset.

> CAIDA dataset: https://www.caida.org/catalog/datasets/passive_dataset/

---

### Accuracy characteristics

| Sketch | Bias | Error type | Best for |
|--------|------|------------|---------|
| CM | Always overcounts | Additive | Heavy hitters, guaranteed upper bounds |
| CU | Always overcounts, less than CM | Additive | Heavy hitters, tighter bounds than CM |
| CS | Unbiased (can over or undercount) | Additive, symmetric | Medium-frequency items |
| RCS | Can under or overcount | Depends on noise distribution | High throughput, lower accuracy acceptable |

### Choosing parameters

**Depth** controls the failure probability. Higher depth = lower probability
of a bad estimate. The paper uses `d=4` for all baselines. Depth of 5 gives
a failure probability of about `(1/e)^5 ≈ 0.7%`.

**Width** controls the error magnitude. Higher width = smaller error. The
error for CM is approximately `e / width * N` where N is the total number
of items inserted.

**l (RCS only)** controls how many counters each flow maps to. The paper uses
`l=50`. Higher l gives better noise distribution but more noise per estimate.
If memory is the constraint, lower l allows more total counters (larger width)
at the cost of worse noise distribution.

A common parameter selection rule for CM/CU/CS:
width = ceil(e / ε)        -- to achieve additive error ε
depth = ceil(ln(1 / δ))    -- to achieve failure probability δ

> Parameter selection reference:
> https://dsf.berkeley.edu/cs286/papers/countmin-latin2004.pdf

---

## References

| Reference | URL |
|-----------|-----|
| SSVS paper (VLDB 2023) | https://doi.org/10.14778/3625054.3625065 |
| SSVS full version (GitHub) | https://github.com/haiporwang/ssvs/blob/main/main.pdf |
| SSVS reference implementation | https://github.com/DimitrisMel/SSVS |
| Count-Min Sketch original paper | https://dsf.berkeley.edu/cs286/papers/countmin-latin2004.pdf |
| Count-Min Sketch Wikipedia | https://en.wikipedia.org/wiki/Count%E2%80%93min_sketch |
| Count Sketch original paper | https://link.springer.com/chapter/10.1007/3-540-45465-9_33 |
| Count Sketch Wikipedia | https://en.wikipedia.org/wiki/Count_sketch |
| Conservative Update (CU) paper | https://dl.acm.org/doi/10.1145/633025.633056 |
| RCS original paper | https://dl.acm.org/doi/10.1145/1851182.1851235 |
| DuckDB official website | https://duckdb.org |
| DuckDB GitHub repository | https://github.com/duckdb/duckdb |
| DuckDB documentation | https://duckdb.org/docs |
| DuckDB extension overview | https://duckdb.org/docs/extensions/overview |
| DuckDB community extensions | https://github.com/duckdb/community-extensions |
| DuckDB extension template | https://github.com/duckdb/extension-template |
| DuckDB community extension development guide | https://duckdb.org/community_extensions/development |
| CAIDA passive dataset | https://www.caida.org/catalog/datasets/passive_dataset/ |
| CMake download | https://cmake.org/download/ |
| Homebrew (macOS) | https://brew.sh |
