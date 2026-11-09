// sketches_extension.cpp
// ──────────────────────────────────────────────────────────────────────────────
// This is the entry point for the DuckDB extension.
//
// When DuckDB loads our extension (LOAD 'sketches'), it calls sketches_init()
// at the bottom of this file, which calls SketchesExtension::Load().
//
// Load() registers every SQL function we implemented with DuckDB's function
// catalog. After that, any SQL query can call countmin_agg(), ssvs_query(),
// etc. just like a built-in function.
//
// Each sketch has 7 functions registered here:
//   xxx_agg()          AGGREGATE  — builds a sketch from a whole table
//   xxx_query()        SCALAR     — estimates the count for one flow
//   xxx_update()       SCALAR     — inserts one flow, returns updated sketch
//   xxx_merge()        SCALAR     — combines two sketches into one
//   xxx_query_timed()  SCALAR     — benchmarks query speed (ns per call)
//   xxx_update_timed() SCALAR     — benchmarks insert speed, one flow repeated
//   xxx_agg_timed()    AGGREGATE  — builds the sketch and times every insert
//                                   over the whole table (as in the paper)
//
// 12 sketches × 7 functions = 84 SQL functions registered total.
// ──────────────────────────────────────────────────────────────────────────────

#include "sketches_extension.hpp"
#include "sketches_aggregate.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension_util.hpp"

namespace duckdb_sketches {

using namespace duckdb;


// Registers the Aggregate Functions with an SQL command(name) and pointer to the function 
static void RegisterAggregate(DuckDB &db, const std::string &sqlName, AggregateFunction (*getFunction)()){

    // Name of the command (countmin_agg)
    AggregateFunctionSet functionSet(sqlName);

    // AcceptAnyFlowKey() allows any datatype to be used a flow key
    // getFunction() returns a pointer to the aggregate function
    functionSet.AddFunction(AcceptAnyFlowKey(getFunction()));

    // DuckDB's built in command to register a function within the database instance 
    ExtensionUtil::RegisterFunction(*db.instance, functionSet);
}

// Registers the Scalar Functions with an SQL command(name) and pointer to the function
static void RegisterScalar(DuckDB &db, const std::string &sqlName, ScalarFunction (*getFunction)()){

    // Name of the command (countmin_query)
    ScalarFunctionSet functionSet(sqlName);

    // AcceptAnyFlowKey() allows any datatype to be used a flow key
    // getFunction() returns a pointer to the scalar function
    functionSet.AddFunction(AcceptAnyFlowKey(getFunction()));

    // DuckDB's built in command to register a function within the database instance 
    ExtensionUtil::RegisterFunction(*db.instance, functionSet);
}

// Loads all the functions when the extension is loaded into DuckDB
void SketchesExtension::Load(DuckDB &db) {

    // Count-Min Sketch - Active Counter  (CM-AC)
    RegisterAggregate(db, "cmac_agg",    GetCMACAggregateFunction);
    RegisterScalar(db,    "cmac_query",   GetCMACQueryFunction);
    RegisterScalar(db,    "cmac_update",  GetCMACUpdateFunction);
    RegisterScalar(db,    "cmac_merge",   GetCMACMergeFunction);
    RegisterScalar(db, "cmac_query_timed",        GetCMACTimedQueryFunction);
    RegisterAggregate(db, "cmac_agg_timed",        GetCMACTimedAggregateFunction);
    RegisterScalar(db, "cmac_update_timed",        GetCMACTimedUpdateFunction);






    RegisterAggregate(db, "countmin_agg",   GetCountMinAggregateFunction);
    RegisterScalar(db,    "countmin_query",  GetCountMinQueryFunction);
    RegisterScalar(db,    "countmin_update", GetCountMinUpdateFunction);
    RegisterScalar(db,    "countmin_merge",  GetCountMinMergeFunction);
    RegisterScalar(db,    "countmin_query_timed",    GetCountMinTimedQueryFunction);
    RegisterAggregate(db, "countmin_agg_timed",    GetCountMinTimedAggregateFunction);
    RegisterScalar(db, "countmin_update_timed",    GetCountMinTimedUpdateFunction);


    
    // ═════════════════════════════════════════════════════════════════════════
    // COUNTER UPDATE SKETCH  (CU)
    // Conservative CM: reads all d counters first, only increments the minimum.
    // Same BLOB format as CM so query/update/merge functions are shared.
    // ════════════════════════════════════
    RegisterAggregate(db, "cu_agg",    GetCUAggregateFunction);
    RegisterScalar(db,    "cu_query",   GetCUQueryFunction);
    RegisterScalar(db,    "cu_update",  GetCUUpdateFunction);
    RegisterScalar(db,    "cu_merge",   GetCUMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // COUNT SKETCH  (CS)
    // Signed counters: each packet adds +1 or -1 based on a hash bit.
    // Query: median of (counter × sign) across d rows.
    // Noise from other flows partially cancels — unbiased estimator.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "countsketch_agg",    GetCountSketchAggregateFunction);
    RegisterScalar(db,    "countsketch_query",   GetCountSketchQueryFunction);
    RegisterScalar(db,    "countsketch_update",  GetCountSketchUpdateFunction);
    RegisterScalar(db,    "countsketch_merge",   GetCountSketchMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // CM WITH SELF-ADJUSTING COUNTERS  (CM-SC)
    // SAC64: packs 4 × 8-bit sub-slots into 36 bits.
    // Same memory holds 3.6× more counters → fewer collisions → lower error.
    // Sub-slots promote to 16-bit or 32-bit on overflow.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "cmsc_agg",    GetCMSCAggregateFunction);
    RegisterScalar(db,    "cmsc_query",   GetCMSCQueryFunction);
    RegisterScalar(db,    "cmsc_update",  GetCMSCUpdateFunction);
    RegisterScalar(db,    "cmsc_merge",   GetCMSCMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // COUNTER UPDATE WITH SELF-ADJUSTING COUNTERS  (CU-SC)
    // Best-accuracy multi-update sketch in the SSVS paper.
    // Conservative update on SAC64 counters.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "cusc_agg",    GetCUSCAggregateFunction);
    RegisterScalar(db,    "cusc_query",   GetCUSCQueryFunction);
    RegisterScalar(db,    "cusc_update",  GetCUSCUpdateFunction);
    RegisterScalar(db,    "cusc_merge",   GetCUSCMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // COUNT SKETCH WITH SELF-ADJUSTING COUNTERS  (CS-SC)
    // Signed SAC64 counters. Sub-slot chosen by 2 hash bits.
    // Query: median of signed sub-slot values.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "cssc_agg",    GetCSSCAggregateFunction);
    RegisterScalar(db,    "cssc_query",   GetCSSCQueryFunction);
    RegisterScalar(db,    "cssc_update",  GetCSSCUpdateFunction);
    RegisterScalar(db,    "cssc_merge",   GetCSSCMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // CM WITH ACTIVE COUNTERS  (CM-AC)
    // AC16: 16-bit probabilistic counter (sign + value + exponent).
    // Actual value = ±(value × 2^exp). Updates fire with probability 1/2^exp.
    // 2× more counters than 32-bit CM for the same memory budget.
    // ═════════════════════════════════════════════════════════════════════════

    // ═════════════════════════════════════════════════════════════════════════
    // COUNTER UPDATE WITH ACTIVE COUNTERS  (CU-AC)
    // Conservative update on AC16 counters.
    // Note: degrades for very large flows (≥10K packets) due to
    // the probabilistic gate firing less often at higher exponents.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "cuac_agg",    GetCUACAggregateFunction);
    RegisterScalar(db,    "cuac_query",   GetCUACQueryFunction);
    RegisterScalar(db,    "cuac_update",  GetCUACUpdateFunction);
    RegisterScalar(db,    "cuac_merge",   GetCUACMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // COUNT SKETCH WITH ACTIVE COUNTERS  (CS-AC)
    // Signed probabilistic AC16 counters. Median query.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "csac_agg",    GetCSACAggregateFunction);
    RegisterScalar(db,    "csac_query",   GetCSACQueryFunction);
    RegisterScalar(db,    "csac_update",  GetCSACUpdateFunction);
    RegisterScalar(db,    "csac_merge",   GetCSACMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // RANDOMIZED COUNTER SHARING  (RCS)
    // Single-update: each packet writes exactly ONE counter chosen randomly.
    // Insert is faster than multi-update sketches (1 write vs d=4).
    // Query: sum all d counters for the flow, subtract expected noise (n/w).
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "rcs_agg",    GetRCSAggregateFunction);
    RegisterScalar(db,    "rcs_query",   GetRCSQueryFunction);
    RegisterScalar(db,    "rcs_update",  GetRCSUpdateFunction);
    RegisterScalar(db,    "rcs_merge",   GetRCSMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // RANDOMIZED COUNTER SHARING WITH ACTIVE COUNTERS  (RCS-AC)
    // Single-update RCS with AC8 (8-bit unsigned probabilistic) counters.
    // AC8 is unsigned only: bits 0-3 = value, bits 4-7 = exponent.
    // ═════════════════════════════════════════════════════════════════════════
    RegisterAggregate(db, "rcsac_agg",    GetRCSACAggregateFunction);
    RegisterScalar(db,    "rcsac_query",   GetRCSACQueryFunction);
    RegisterScalar(db,    "rcsac_update",  GetRCSACUpdateFunction);
    RegisterScalar(db,    "rcsac_merge",   GetRCSACMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // SSVS-2  —  Single Update Sketch with Variable Counter Structure
    // The paper's main contribution. Single-update speed, multi-update accuracy.
    // Variable Counter (VC): starts as two 8-bit bytes, promotes through
    // 16-bit short → small AC → large AC on overflow.
    // Query uses min-gap algorithm: sort 4 estimates, find most consistent pair,
    // discard outliers beyond noise/4, return scaled average of kept estimates.
    // ═════════════════════════════════════════════════════════════════════════

    // SSVS takes up to 3 trailing sizing constants: ssvs_agg(src, dst [, l [, m [, k]]])
    RegisterAggregate(db, "ssvs_agg", GetSSVSAggregateFunction);

    RegisterScalar(db, "ssvs_query",   GetSSVSQueryFunction);
    RegisterScalar(db, "ssvs_update",  GetSSVSUpdateFunction);
    RegisterScalar(db, "ssvs_merge",   GetSSVSMergeFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // TIMED QUERY FUNCTIONS
    // xxx_query_timed(sketch, key...) — each timed run hashes and queries the
    // same flow 10,000 times; 5 runs. Returns:
    //   STRUCT(estimate BIGINT, query_ns DOUBLE, query_ns_min DOUBLE,
    //          queries_per_run INTEGER, hash_ns DOUBLE)
    // estimate     = the normal xxx_query() answer
    // query_ns     = median nanoseconds per query (hash + counter lookup)
    // query_ns_min = fastest of the 5 runs
    // hash_ns      = time to hash the key alone; query_ns - hash_ns = lookup
    // ═════════════════════════════════════════════════════════════════════════

    RegisterScalar(db, "cu_query_timed",          GetCUTimedQueryFunction);
    RegisterScalar(db, "countsketch_query_timed", GetCountSketchTimedQueryFunction);
    RegisterScalar(db, "cmsc_query_timed",        GetCMSCTimedQueryFunction);
    RegisterScalar(db, "cusc_query_timed",        GetCUSCTimedQueryFunction);
    RegisterScalar(db, "cssc_query_timed",        GetCSSCTimedQueryFunction);

    RegisterScalar(db, "cuac_query_timed",        GetCUACTimedQueryFunction);
    RegisterScalar(db, "csac_query_timed",        GetCSACTimedQueryFunction);
    RegisterScalar(db, "rcs_query_timed",         GetRCSTimedQueryFunction);
    RegisterScalar(db, "rcsac_query_timed",       GetRCSACTimedQueryFunction);
    RegisterScalar(db, "ssvs_query_timed",        GetSSVSTimedQueryFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // TIMED UPDATE FUNCTIONS
    // xxx_update_timed(sketch, key...) — each timed run hashes and inserts the
    // same flow 100,000 times into a copy of the sketch; 5 runs. Returns:
    //   STRUCT(sketch BLOB, insert_ns DOUBLE, insert_ns_min DOUBLE,
    //          inserts_per_run INTEGER, hash_ns DOUBLE, hashes_per_insert DOUBLE)
    // The original sketch is returned unchanged.
    // insert_ns         = median nanoseconds per insert (key hashes + counter update)
    // insert_ns_min     = fastest of the 5 runs
    // hash_ns           = time for ONE key hash
    // hashes_per_insert = key hashes per insert (1 for RCS/RCS-AC/SSVS, d or more
    //                     for the others, as in the Java code)
    // insert_ns - hash_ns * hashes_per_insert = counter update alone
    // ═════════════════════════════════════════════════════════════════════════

    RegisterScalar(db, "cu_update_timed",          GetCUTimedUpdateFunction);
    RegisterScalar(db, "countsketch_update_timed", GetCountSketchTimedUpdateFunction);
    RegisterScalar(db, "cmsc_update_timed",        GetCMSCTimedUpdateFunction);
    RegisterScalar(db, "cusc_update_timed",        GetCUSCTimedUpdateFunction);
    RegisterScalar(db, "cssc_update_timed",        GetCSSCTimedUpdateFunction);

    RegisterScalar(db, "cuac_update_timed",        GetCUACTimedUpdateFunction);
    RegisterScalar(db, "csac_update_timed",        GetCSACTimedUpdateFunction);
    RegisterScalar(db, "rcs_update_timed",         GetRCSTimedUpdateFunction);
    RegisterScalar(db, "rcsac_update_timed",       GetRCSACTimedUpdateFunction);
    RegisterScalar(db, "ssvs_update_timed",        GetSSVSTimedUpdateFunction);

    // ═════════════════════════════════════════════════════════════════════════
    // WHOLE-TRACE TIMED AGGREGATES  (how the paper measured speed)
    // xxx_agg_timed(key... [, sizing constants]) builds the same sketch as
    // xxx_agg() and times every insert, in table order. Run with
    // SET threads = 1 for a single sketch filled in trace order. Returns:
    //   STRUCT(sketch BLOB, insert_ns DOUBLE, packets BIGINT, total_ms DOUBLE)
    // insert_ns = total insert time / packets (reading the table and joining
    //             the key columns are not timed)
    // ═════════════════════════════════════════════════════════════════════════

    RegisterAggregate(db, "cu_agg_timed",          GetCUTimedAggregateFunction);
    RegisterAggregate(db, "countsketch_agg_timed", GetCountSketchTimedAggregateFunction);
    RegisterAggregate(db, "cmsc_agg_timed",        GetCMSCTimedAggregateFunction);
    RegisterAggregate(db, "cusc_agg_timed",        GetCUSCTimedAggregateFunction);
    RegisterAggregate(db, "cssc_agg_timed",        GetCSSCTimedAggregateFunction);

    RegisterAggregate(db, "cuac_agg_timed",        GetCUACTimedAggregateFunction);
    RegisterAggregate(db, "csac_agg_timed",        GetCSACTimedAggregateFunction);
    RegisterAggregate(db, "rcs_agg_timed",         GetRCSTimedAggregateFunction);
    RegisterAggregate(db, "rcsac_agg_timed",       GetRCSACTimedAggregateFunction);
    RegisterAggregate(db, "ssvs_agg_timed",        GetSSVSTimedAggregateFunction);
}

// The name DuckDB uses to identify this extension in the catalog
std::string SketchesExtension::Name() { return "sketches"; }

// Version string returned by duckdb_extensions()
std::string SketchesExtension::Version() const { return "v0.1.0"; }

} // namespace duckdb_sketches

// ─────────────────────────────────────────────────────────────────────────────
// C ENTRY POINTS
//
// DuckDB loads extensions as shared libraries (.duckdb_extension files)
// and calls these two C functions by name after dlopen().
// They must be extern "C" so the linker does not mangle their names.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

// Called once when the extension is loaded.
// Wraps the raw DatabaseInstance in a DuckDB object and delegates to Load().
DUCKDB_EXTENSION_API void sketches_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB dbWrapper(db);
    duckdb_sketches::SketchesExtension extension;
    extension.Load(dbWrapper);
}

// Returns the DuckDB version this extension was compiled against.
// DuckDB checks this to verify ABI compatibility before loading.
DUCKDB_EXTENSION_API const char *sketches_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"