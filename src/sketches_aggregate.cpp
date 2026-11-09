// sketches_aggregate.cpp
// ──────────────────────────────────────────────────────────────────────────────
// This file implements all the SQL functions for every sketch.
//
// For each sketch there are four functions:
//   xxx_agg(key...)           AGGREGATE  — builds a sketch from a whole table
//   xxx_query(blob, key...)   SCALAR     — estimates count of one flow
//   xxx_update(blob, key...)  SCALAR     — inserts one flow, returns new sketch
//   xxx_merge(blob1, blob2)   SCALAR     — combines two sketches into one
//
// plus the speed-measurement functions at the end of this file:
//   xxx_update_timed(blob, key...)  one flow inserted 100,000 times per run
//   xxx_query_timed(blob, key...)   one flow queried 10,000 times per run
//   xxx_agg_timed(key... [, sizes]) builds the xxx_agg sketch and times every
//                                   insert over the whole table (as in the paper)
//
// key... is one or more columns of any type, e.g. (src, dst). See FLOW KEYS below.
//
// The aggregate function is handled by the Operation/Combine/Finalize callbacks
// defined in the header file.  The three scalar functions are defined here.
//
// Pattern for every scalar function:
//   1. Loop over every row in the batch DuckDB gave us
//   2. Skip any row where an input is NULL
//   3. Deserialize the sketch BLOB back into a C++ object
//   4. Hash the flow string
//   5. Do the operation (query / insert / merge)
//   6. Write the result back into the output column
// ──────────────────────────────────────────────────────────────────────────────
#include "sketches_aggregate.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/function_binder.hpp"
#include <chrono>

namespace duckdb_sketches {
using namespace duckdb;


// ═════════════════════════════════════════════════════════════════════════════
// FLOW KEYS — any number of columns, of any data type
//
// Every sketch hashes one string per flow. These helpers let SQL pass the flow
// as one or more columns of ANY type, with no casts needed:
//
//     countmin_agg(src, dst)                   countmin_query(sketch, src, dst)
//     countmin_agg(src, dst, 4, 8192)          countmin_query(sketch, '10.0.0.1', '8.8.8.8')
//     countmin_agg(src_ip_int, dst_ip_int)     countmin_query(sketch, 167772161, 134744072)
//
// At bind time each key column is cast to VARCHAR and the columns are joined
// with a TAB, so the aggregate and the query always hash the same bytes for the
// same flow, whatever the column types are. (The Java reference joins the fields
// of a flow ID with a TAB too, so (src, dst) hashes exactly like Java's
// "src\tdst".) A single VARCHAR column is passed through unchanged, so sketches
// built with the old one-string form still answer queries made the old way.
//
// Use the SAME columns, in the SAME order, when building and when querying.
// If any key column is NULL the whole key is NULL: the aggregate skips the row
// and the query returns NULL.
// ═════════════════════════════════════════════════════════════════════════════

// Casts each key column to VARCHAR and joins them as  c1 || '\t' || c2 || ...
static unique_ptr<Expression> BuildFlowKey(ClientContext &context,
                                           vector<unique_ptr<Expression>> parts) {
    if (parts.empty())
        throw BinderException("sketches: expected at least one flow key column");
    for (auto &part : parts)
        part = BoundCastExpression::AddCastToType(context, std::move(part), LogicalType::VARCHAR);
    FunctionBinder binder(context);
    unique_ptr<Expression> key = std::move(parts[0]);
    for (idx_t i = 1; i < parts.size(); i++) {
        for (int step = 0; step < 2; step++) {          // key || '\t', then key || part
            vector<unique_ptr<Expression>> children;
            children.push_back(std::move(key));
            if (step == 0) children.push_back(make_uniq<BoundConstantExpression>(Value("\t")));
            else           children.push_back(std::move(parts[i]));
            ErrorData error;
            key = binder.BindScalarFunction(DEFAULT_SCHEMA, "||", std::move(children), error, true);
            if (!key) error.Throw();
        }
    }
    return BoundCastExpression::AddCastToType(context, std::move(key), LogicalType::VARCHAR);
}

// Aggregates: xxx_agg(key1 [, key2 ...] [, param1 [, param2 [, param3]]])
// Trailing integer CONSTANTS (up to maxParams) are the sizing parameters
// (depth, width, ...); everything before them is the flow key. Rewrites the
// call so the aggregate receives exactly one VARCHAR key, and returns the
// sizing parameters in order.
static vector<int32_t> BindFlowKeyAggregate(ClientContext &context, AggregateFunction &function,
                                            vector<unique_ptr<Expression>> &args, idx_t maxParams) {
    idx_t numParams = 0;
    while (numParams < maxParams && args.size() - numParams > 1) {
        auto &arg = args[args.size() - 1 - numParams];
        bool isInteger = arg->return_type.IsIntegral() ||
                         arg->return_type.id() == LogicalTypeId::INTEGER_LITERAL;
        if (!isInteger || !arg->IsFoldable()) break;
        numParams++;
    }
    idx_t numKeys = args.size() - numParams;
    vector<int32_t> params;
    for (idx_t i = numKeys; i < args.size(); i++) {
        Value v = ExpressionExecutor::EvaluateScalar(context, *args[i]);
        if (v.IsNull()) throw BinderException(function.name + ": sizing parameters cannot be NULL");
        params.push_back(v.GetValue<int32_t>());
    }
    vector<unique_ptr<Expression>> parts;
    for (idx_t i = 0; i < numKeys; i++) parts.push_back(std::move(args[i]));
    args.clear();
    args.push_back(BuildFlowKey(context, std::move(parts)));
    function.arguments = {LogicalType::VARCHAR};
    function.varargs = LogicalType::INVALID;
    return params;
}

// Scalars: xxx_query / xxx_update / xxx_query_timed / xxx_update_timed
//          (sketch BLOB, key1 [, key2 ...])
static unique_ptr<FunctionData> BindFlowKeyScalar(ClientContext &context, ScalarFunction &function,
                                                  vector<unique_ptr<Expression>> &args) {
    vector<unique_ptr<Expression>> parts;
    for (idx_t i = 1; i < args.size(); i++) parts.push_back(std::move(args[i]));
    args.resize(1);
    args.push_back(BuildFlowKey(context, std::move(parts)));
    function.arguments = {LogicalType::BLOB, LogicalType::VARCHAR};
    function.varargs = LogicalType::INVALID;
    return nullptr;
}

AggregateFunction AcceptAnyFlowKey(AggregateFunction function) {
    function.arguments = {LogicalType::ANY};   // first key column
    function.varargs = LogicalType::ANY;       // more key columns and/or sizing constants
    return function;
}

ScalarFunction AcceptAnyFlowKey(ScalarFunction function) {
    // Only functions shaped (BLOB, VARCHAR) take a flow key; merges (BLOB, BLOB) are left alone.
    if (function.arguments.size() == 2 && function.arguments[0] == LogicalType::BLOB &&
        function.arguments[1] == LogicalType::VARCHAR) {
        function.arguments = {LogicalType::BLOB, LogicalType::ANY};
        function.varargs = LogicalType::ANY;
        function.bind = BindFlowKeyScalar;
    }
    return function;
}


// ═════════════════════════════════════════════════════════════════════════════
// BIND FUNCTIONS
//
// A bind function runs once at query-planning time (before any rows arrive).
// It reads the depth and width parameters the user passed to the aggregate,
// validates them, and packages them into a BindData object that all worker
// threads will share.  The seeds are generated here too so every thread
// builds a compatible sketch that can be merged later.
// ═════════════════════════════════════════════════════════════════════════════

// Helper that creates the BindData for any CM-family sketch.
// conservative=false → CM (Count-Min)
// conservative=true  → CU (Counter Update)
static unique_ptr<FunctionData> BindCountMinFamily(
    ClientContext &context,
    AggregateFunction &function,
    vector<unique_ptr<Expression>> &arguments,
    const string &functionName,
    bool conservative) {

    // Default sketch dimensions — fills exactly 1 Mbit with 32-bit counters
    int32_t depth = 4;
    int32_t width = 8192;

    // Split the arguments into flow-key columns and trailing constants
    // (depth, width), and turn the key columns into one VARCHAR key.
    auto params = BindFlowKeyAggregate(context, function, arguments, 2);
    if (params.size() >= 1) depth = params[0];
    if (params.size() >= 2) width = params[1];

    // Validate that the dimensions are reasonable
    if (depth < 1 || depth > 64)
        throw BinderException(functionName + ": depth must be between 1 and 64");
    if (width < 8 || width > (1 << 24))
        throw BinderException(functionName + ": width must be between 8 and 16M");

    // Package depth, width, conservative flag, and freshly-generated seeds
    // into a BindData object.  All threads share this same object.
    return make_uniq<CountMinBindData>(depth, width, conservative);
}

static unique_ptr<FunctionData> CountMinBind(
    ClientContext &ctx, AggregateFunction &fn,
    vector<unique_ptr<Expression>> &args) {
    return BindCountMinFamily(ctx, fn, args, "countmin_agg", false);
}

static unique_ptr<FunctionData> CountMinConservativeBind(
    ClientContext &ctx, AggregateFunction &fn,
    vector<unique_ptr<Expression>> &args) {
    return BindCountMinFamily(ctx, fn, args, "cu_agg", true);
}

// Aggregate factory — wraps the Operation/Combine/Finalize callbacks
// from the header into a DuckDB AggregateFunction object.
AggregateFunction GetCountMinAggregateFunction() {
    auto aggregateFunction = AggregateFunction::UnaryAggregate<
        CountMinState, string_t, string_t, CountMinOperation>(
            LogicalType::VARCHAR, LogicalType::BLOB);
    aggregateFunction.name        = "countmin_agg";
    aggregateFunction.bind        = CountMinBind;
    aggregateFunction.destructor  = AggregateFunction::StateDestroy<
        CountMinState, CountMinOperation>;
    return aggregateFunction;
}

AggregateFunction GetCUAggregateFunction() {
    auto aggregateFunction = AggregateFunction::UnaryAggregate<
        CountMinState, string_t, string_t, CountMinOperation>(
            LogicalType::VARCHAR, LogicalType::BLOB);
    aggregateFunction.name        = "cu_agg";
    aggregateFunction.bind        = CountMinConservativeBind;
    aggregateFunction.destructor  = AggregateFunction::StateDestroy<
        CountMinState, CountMinOperation>;
    return aggregateFunction;
}


// ═══════════════════════════════════════════════════════════════════════
// COUNT-MIN SKETCH  (CM)  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// countmin_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────


// This tells DuckDB how to process a batch of rows for the countmin_query() function each 
// row being a flow to query. 
// It says what the inputs are (the sketch and the flow) as well as a place to put the output
// a vector because we're processing multiple flows at once. 
static void countminQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows (flows) are in the input (capped at 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);


    // Gets the two input columns 0 being the sketch and 1 being the flows 
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];


    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CountMinSketch sketch = CountMinSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

// Gives DuckDB all the information it needs to run countminQuery 
ScalarFunction GetCountMinQueryFunction(){
    
    // Tell DuckDB this SQL function is called "countmin_query",
    // inputs are LogicalType::BLOB and LogicalType::VARCHAR and the output is a LogicalType::BIGINT
    // and it calls countminQuery to do the work (defined above)
    return ScalarFunction("countmin_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        countminQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// countmin_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void countminUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CountMinSketch sketch = CountMinSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCountMinUpdateFunction() {
    // Tell DuckDB: this SQL function is called "countmin_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls countminUpdate to do the work.
    return ScalarFunction("countmin_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        countminUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// countmin_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void countminMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CountMinSketch firstSketch  = CountMinSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CountMinSketch secondSketch = CountMinSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCountMinMergeFunction() {
    // Tell DuckDB: this SQL function is called "countmin_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls countminMerge to do the work.
    return ScalarFunction("countmin_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        countminMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// COUNTER UPDATE SKETCH  (CU)  —  conservative CM, same structure
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cu_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void cuQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CountMinSketch sketch = CountMinSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCUQueryFunction() {
    // Tell DuckDB: this SQL function is called "cu_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls cuQuery to do the work.
    return ScalarFunction("cu_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        cuQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cu_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void cuUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CountMinSketch sketch = CountMinSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCUUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cu_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls cuUpdate to do the work.
    return ScalarFunction("cu_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        cuUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cu_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void cuMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CountMinSketch firstSketch  = CountMinSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CountMinSketch secondSketch = CountMinSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCUMergeFunction() {
    // Tell DuckDB: this SQL function is called "cu_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls cuMerge to do the work.
    return ScalarFunction("cu_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        cuMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// COUNT SKETCH  (CS)  —  signed counters, median query
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// countsketch_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────

// CountSketch aggregate bind + factory
static unique_ptr<FunctionData> CountSketchBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=8192;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("countsketch_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("countsketch_agg: width 8-16M");
    return make_uniq<CountSketchBindData>(depth,width);}

AggregateFunction GetCountSketchAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CountSketchState,string_t,string_t,CountSketchOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="countsketch_agg"; f.bind=CountSketchBind;
    f.destructor=AggregateFunction::StateDestroy<CountSketchState,CountSketchOperation>;
    return f;}

static void countSketchQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CountSketch sketch = CountSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCountSketchQueryFunction() {
    // Tell DuckDB: this SQL function is called "countsketch_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls countSketchQuery to do the work.
    return ScalarFunction("countsketch_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        countSketchQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// countsketch_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void countSketchUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CountSketch sketch = CountSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCountSketchUpdateFunction() {
    // Tell DuckDB: this SQL function is called "countsketch_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls countSketchUpdate to do the work.
    return ScalarFunction("countsketch_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        countSketchUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// countsketch_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void countSketchMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CountSketch firstSketch  = CountSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CountSketch secondSketch = CountSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCountSketchMergeFunction() {
    // Tell DuckDB: this SQL function is called "countsketch_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls countSketchMerge to do the work.
    return ScalarFunction("countsketch_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        countSketchMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CMSC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cmsc_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void cmscQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CMSCSketch sketch = CMSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCMSCQueryFunction() {
    // Tell DuckDB: this SQL function is called "cmsc_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls cmscQuery to do the work.
    return ScalarFunction("cmsc_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        cmscQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cmsc_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void cmscUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CMSCSketch sketch = CMSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCMSCUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cmsc_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls cmscUpdate to do the work.
    return ScalarFunction("cmsc_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        cmscUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cmsc_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void cmscMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CMSCSketch firstSketch  = CMSCSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CMSCSketch secondSketch = CMSCSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCMSCMergeFunction() {
    // Tell DuckDB: this SQL function is called "cmsc_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls cmscMerge to do the work.
    return ScalarFunction("cmsc_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        cmscMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CUSC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cusc_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void cuscQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CMSCSketch sketch = CMSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCUSCQueryFunction() {
    // Tell DuckDB: this SQL function is called "cusc_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls cuscQuery to do the work.
    return ScalarFunction("cusc_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        cuscQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cusc_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void cuscUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CMSCSketch sketch = CMSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCUSCUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cusc_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls cuscUpdate to do the work.
    return ScalarFunction("cusc_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        cuscUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cusc_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void cuscMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CMSCSketch firstSketch  = CMSCSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CMSCSketch secondSketch = CMSCSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCUSCMergeFunction() {
    // Tell DuckDB: this SQL function is called "cusc_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls cuscMerge to do the work.
    return ScalarFunction("cusc_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        cuscMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CSSC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cssc_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void csscQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CSSCSketch sketch = CSSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCSSCQueryFunction() {
    // Tell DuckDB: this SQL function is called "cssc_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls csscQuery to do the work.
    return ScalarFunction("cssc_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        csscQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cssc_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void csscUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CSSCSketch sketch = CSSCSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCSSCUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cssc_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls csscUpdate to do the work.
    return ScalarFunction("cssc_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        csscUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cssc_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void csscMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CSSCSketch firstSketch  = CSSCSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CSSCSketch secondSketch = CSSCSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCSSCMergeFunction() {
    // Tell DuckDB: this SQL function is called "cssc_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls csscMerge to do the work.
    return ScalarFunction("cssc_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        csscMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CMAC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cmac_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────


static void cmacQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CMACSketch sketch = CMACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCMACQueryFunction() {
    // Tell DuckDB: this SQL function is called "cmac_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // cmacQuery allows for multiple flows to be processed at once before sending to query function
    return ScalarFunction("cmac_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        cmacQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cmac_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void cmacUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CMACSketch sketch = CMACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCMACUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cmac_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls cmacUpdate to do the work.
    return ScalarFunction("cmac_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        cmacUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cmac_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void cmacMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CMACSketch firstSketch  = CMACSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CMACSketch secondSketch = CMACSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCMACMergeFunction() {
    // Tell DuckDB: this SQL function is called "cmac_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls cmacMerge to do the work.
    return ScalarFunction("cmac_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        cmacMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CUAC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// cuac_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void cuacQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CMACSketch sketch = CMACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCUACQueryFunction() {
    // Tell DuckDB: this SQL function is called "cuac_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls cuacQuery to do the work.
    return ScalarFunction("cuac_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        cuacQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// cuac_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void cuacUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CMACSketch sketch = CMACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCUACUpdateFunction() {
    // Tell DuckDB: this SQL function is called "cuac_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls cuacUpdate to do the work.
    return ScalarFunction("cuac_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        cuacUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// cuac_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void cuacMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CMACSketch firstSketch  = CMACSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CMACSketch secondSketch = CMACSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCUACMergeFunction() {
    // Tell DuckDB: this SQL function is called "cuac_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls cuacMerge to do the work.
    return ScalarFunction("cuac_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        cuacMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// CSAC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// csac_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void csacQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        CSACSketch sketch = CSACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetCSACQueryFunction() {
    // Tell DuckDB: this SQL function is called "csac_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls csacQuery to do the work.
    return ScalarFunction("csac_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        csacQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// csac_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void csacUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        CSACSketch sketch = CSACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key, the same way the aggregate does)
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()));

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetCSACUpdateFunction() {
    // Tell DuckDB: this SQL function is called "csac_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls csacUpdate to do the work.
    return ScalarFunction("csac_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        csacUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// csac_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void csacMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        CSACSketch firstSketch  = CSACSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        CSACSketch secondSketch = CSACSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetCSACMergeFunction() {
    // Tell DuckDB: this SQL function is called "csac_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls csacMerge to do the work.
    return ScalarFunction("csac_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        csacMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// RCS SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// rcs_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        RCSSketch sketch = RCSSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetRCSQueryFunction() {
    // Tell DuckDB: this SQL function is called "rcs_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls rcsQuery to do the work.
    return ScalarFunction("rcs_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        rcsQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// rcs_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        RCSSketch sketch = RCSSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key and picks a random row)
        uint64_t randomState = RandomSeed();
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()), randomState);

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetRCSUpdateFunction() {
    // Tell DuckDB: this SQL function is called "rcs_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls rcsUpdate to do the work.
    return ScalarFunction("rcs_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        rcsUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// rcs_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        RCSSketch firstSketch  = RCSSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        RCSSketch secondSketch = RCSSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetRCSMergeFunction() {
    // Tell DuckDB: this SQL function is called "rcs_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls rcsMerge to do the work.
    return ScalarFunction("rcs_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        rcsMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// RCS-AC SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// rcsac_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsacQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        RCSACSketch sketch = RCSACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetRCSACQueryFunction() {
    // Tell DuckDB: this SQL function is called "rcsac_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls rcsacQuery to do the work.
    return ScalarFunction("rcsac_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        rcsacQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// rcsac_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsacUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        RCSACSketch sketch = RCSACSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key and picks a random row)
        uint64_t randomState = RandomSeed();
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()), randomState);

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetRCSACUpdateFunction() {
    // Tell DuckDB: this SQL function is called "rcsac_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls rcsacUpdate to do the work.
    return ScalarFunction("rcsac_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        rcsacUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// rcsac_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void rcsacMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        RCSACSketch firstSketch  = RCSACSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        RCSACSketch secondSketch = RCSACSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetRCSACMergeFunction() {
    // Tell DuckDB: this SQL function is called "rcsac_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls rcsacMerge to do the work.
    return ScalarFunction("rcsac_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        rcsacMerge);
}

// ═══════════════════════════════════════════════════════════════════════
// SSVS-2 SKETCH  —  scalar functions
// ═══════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ssvs_query(sketchBlob BLOB,  flowString VARCHAR)  →  BIGINT
//
// Given a serialized sketch and a flow string, returns the estimated
// packet count for that flow.
// ─────────────────────────────────────────────────────────────────────────────
static void ssvsQuery(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    // How many rows DuckDB wants us to process in this batch (up to 2048)
    int numberOfRows = inputs.size();

    // Get a pointer to the output array so we can write one int64 per row
    int64_t *outputValues = FlatVector::GetData<int64_t>(outputColumn);

    // Pull out the two input columns
    //   column 0 = the sketch BLOB
    //   column 1 = the flow string (e.g. "10.0.0.1->8.8.8.8")
    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    // Convert both columns into a uniform format DuckDB can iterate
    // (columns can be stored in different internal layouts; this normalises them)
    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    // Tell DuckDB the output is a plain flat array (one value per row)
    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);

    // Get the validity bitmap so we can mark NULL outputs
    auto &outputValidity = FlatVector::Validity(outputColumn);

    // Process each row one at a time
    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        // Look up where this row's actual data lives in the column storage
        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        // If either input is NULL, mark the output as NULL and skip this row
        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        // Extract the raw bytes of the sketch BLOB for this row
        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];

        // Extract the flow string for this row (e.g. "192.168.1.1->10.0.0.2")
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch object from its serialized bytes
        SSVSSketch sketch = SSVSSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Hash the flow string — same hash used at insert time
        // so it maps to the exact same counter positions
        int32_t flowHash = FNVHash1(flowString.GetData(), flowString.GetSize());

        // Ask the sketch for its estimated count and write it to the output
        outputValues[rowIndex] = sketch.Query(flowHash);
    }
}

ScalarFunction GetSSVSQueryFunction() {
    // Tell DuckDB: this SQL function is called "ssvs_query",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BIGINT,
    // and calls ssvsQuery to do the work.
    return ScalarFunction("ssvs_query",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BIGINT,
        ssvsQuery);
}

// ─────────────────────────────────────────────────────────────────────────────
// ssvs_update(sketchBlob BLOB,  flowString VARCHAR)  →  BLOB
//
// Inserts one flow into the sketch and returns the updated sketch as a BLOB.
// Use countmin_agg() to build a sketch from a whole table — this function
// is only for adding a single flow to an already-built sketch.
// ─────────────────────────────────────────────────────────────────────────────
static void ssvsUpdate(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &sketchColumn = inputs.data[0];
    Vector &flowColumn   = inputs.data[1];

    UnifiedVectorFormat sketchColumnData, flowColumnData;
    sketchColumn.ToUnifiedFormat(numberOfRows, sketchColumnData);
    flowColumn.ToUnifiedFormat(numberOfRows, flowColumnData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int sketchDataIndex = sketchColumnData.sel->get_index(rowIndex);
        int flowDataIndex   = flowColumnData.sel->get_index(rowIndex);

        bool sketchIsNull = !sketchColumnData.validity.RowIsValid(sketchDataIndex);
        bool flowIsNull   = !flowColumnData.validity.RowIsValid(flowDataIndex);
        if (sketchIsNull || flowIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t sketchBlob = ((string_t*)sketchColumnData.data)[sketchDataIndex];
        string_t flowString = ((string_t*)flowColumnData.data)[flowDataIndex];

        // Rebuild the sketch from its serialized bytes
        SSVSSketch sketch = SSVSSketch::Deserialize(
            sketchBlob.GetData(), sketchBlob.GetSize());

        // Insert the flow (the sketch hashes the key and picks a random row)
        uint64_t randomState = RandomSeed();
        sketch.Insert(FlowKey(flowString.GetData(), flowString.GetSize()), randomState);

        // Serialize the updated sketch back to bytes so DuckDB can store it
        std::string updatedSketchBytes = sketch.Serialize();

        // Write the new BLOB into the output column for this row
        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                updatedSketchBytes.data(),
                updatedSketchBytes.size());
    }
}

ScalarFunction GetSSVSUpdateFunction() {
    // Tell DuckDB: this SQL function is called "ssvs_update",
    // takes LogicalType::BLOB, LogicalType::VARCHAR as input, returns LogicalType::BLOB,
    // and calls ssvsUpdate to do the work.
    return ScalarFunction("ssvs_update",
        {LogicalType::BLOB, LogicalType::VARCHAR},
        LogicalType::BLOB,
        ssvsUpdate);
}

// ─────────────────────────────────────────────────────────────────────────────
// ssvs_merge(sketchBlob1 BLOB,  sketchBlob2 BLOB)  →  BLOB
//
// Combines two sketches by adding their counters cell-by-cell.
// Both sketches must have been built with the same seeds.
// ─────────────────────────────────────────────────────────────────────────────
static void ssvsMerge(DataChunk &inputs, ExpressionState &, Vector &outputColumn) {

    int numberOfRows = inputs.size();

    Vector &firstSketchColumn  = inputs.data[0];
    Vector &secondSketchColumn = inputs.data[1];

    UnifiedVectorFormat firstSketchData, secondSketchData;
    firstSketchColumn.ToUnifiedFormat(numberOfRows, firstSketchData);
    secondSketchColumn.ToUnifiedFormat(numberOfRows, secondSketchData);

    outputColumn.SetVectorType(VectorType::FLAT_VECTOR);
    auto &outputValidity = FlatVector::Validity(outputColumn);

    for (int rowIndex = 0; rowIndex < numberOfRows; rowIndex++) {

        int firstIndex  = firstSketchData.sel->get_index(rowIndex);
        int secondIndex = secondSketchData.sel->get_index(rowIndex);

        bool firstIsNull  = !firstSketchData.validity.RowIsValid(firstIndex);
        bool secondIsNull = !secondSketchData.validity.RowIsValid(secondIndex);
        if (firstIsNull || secondIsNull) {
            outputValidity.SetInvalid(rowIndex);
            continue;
        }

        string_t firstBlob  = ((string_t*)firstSketchData.data)[firstIndex];
        string_t secondBlob = ((string_t*)secondSketchData.data)[secondIndex];

        // Rebuild both sketches from their serialized bytes
        SSVSSketch firstSketch  = SSVSSketch::Deserialize(
            firstBlob.GetData(),  firstBlob.GetSize());
        SSVSSketch secondSketch = SSVSSketch::Deserialize(
            secondBlob.GetData(), secondBlob.GetSize());

        // Add every counter in secondSketch into firstSketch
        firstSketch.Merge(secondSketch);

        // Serialize the merged result back to bytes
        std::string mergedBytes = firstSketch.Serialize();

        FlatVector::GetData<string_t>(outputColumn)[rowIndex] =
            StringVector::AddStringOrBlob(
                outputColumn,
                mergedBytes.data(),
                mergedBytes.size());
    }
}

ScalarFunction GetSSVSMergeFunction() {
    // Tell DuckDB: this SQL function is called "ssvs_merge",
    // takes LogicalType::BLOB, LogicalType::BLOB as input, returns LogicalType::BLOB,
    // and calls ssvsMerge to do the work.
    return ScalarFunction("ssvs_merge",
        {LogicalType::BLOB, LogicalType::BLOB},
        LogicalType::BLOB,
        ssvsMerge);
}

// ═════════════════════════════════════════════════════════════════════════════
// AGGREGATE BIND + FACTORY FUNCTIONS  (extracted from original)
// ═════════════════════════════════════════════════════════════════════════════
// ── CU (Counter Update, conservative CM) ═══════════════════════════════════════════════════════════════

static unique_ptr<FunctionData> CMSCBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=7281;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("cmsc_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("cmsc_agg: width 8-16M");
    return make_uniq<CMSCBindData>(depth,width, false);}

AggregateFunction GetCMSCAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CMSCState,string_t,string_t,CMSCOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="cmsc_agg"; f.bind=CMSCBind;
    f.destructor=AggregateFunction::StateDestroy<CMSCState,CMSCOperation>;
    return f;}




// ── CU (Counter Update, conservative CM)SC ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> CUSCBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=7281;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("cusc_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("cusc_agg: width 8-16M");
    return make_uniq<CMSCBindData>(depth,width, true);}

AggregateFunction GetCUSCAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CMSCState,string_t,string_t,CMSCOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="cusc_agg"; f.bind=CUSCBind;
    f.destructor=AggregateFunction::StateDestroy<CMSCState,CMSCOperation>;
    return f;}




// ── CS-SC (Count Sketch with Self-Adjusting Counters) ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> CSSCBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=7281;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("cssc_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("cssc_agg: width 8-16M");
    return make_uniq<CSSCBindData>(depth,width);}

AggregateFunction GetCSSCAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CSSCState,string_t,string_t,CSSCOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="cssc_agg"; f.bind=CSSCBind;
    f.destructor=AggregateFunction::StateDestroy<CSSCState,CSSCOperation>;
    return f;}




// ── CM-AC (Count-Min with Active Counters) ═══════════════════════════════════════════════════════════════


static unique_ptr<FunctionData> CMACBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    
    // Default parameters
    int32_t depth=4,width=16384;

    // Bind the aggregate function and extract the varchar from the flow pair
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);

    // If the user provided parameters, override the defaults
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];

    // Validate the parameters
    if(depth<1||depth>64) throw BinderException("cmac_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("cmac_agg: width 8-16M");

    // Return a unique pointer to the bind data for the aggregate function
    return make_uniq<CMACBindData>(depth,width, false);}

AggregateFunction GetCMACAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CMACState,string_t,string_t,CMACOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);

    // Set the name, bind function, and destructor for the aggregate function
    f.name="cmac_agg"; f.bind=CMACBind;
    f.destructor=AggregateFunction::StateDestroy<CMACState,CMACOperation>;
    return f;}




// ── CU (Counter Update, conservative CM)AC ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> CUACBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=16384;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("cuac_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("cuac_agg: width 8-16M");
    return make_uniq<CMACBindData>(depth,width, true);}

AggregateFunction GetCUACAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CMACState,string_t,string_t,CMACOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="cuac_agg"; f.bind=CUACBind;
    f.destructor=AggregateFunction::StateDestroy<CMACState,CMACOperation>;
    return f;}




// ── CS-AC (Count Sketch with Active Counters) ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> CSACBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t depth=4,width=16384;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) depth=params[0];
    if(params.size()>=2) width=params[1];
    if(depth<1||depth>64) throw BinderException("csac_agg: depth 1-64");
    if(width<8||width>(1<<24)) throw BinderException("csac_agg: width 8-16M");
    return make_uniq<CSACBindData>(depth,width);}

AggregateFunction GetCSACAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<CSACState,string_t,string_t,CSACOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="csac_agg"; f.bind=CSACBind;
    f.destructor=AggregateFunction::StateDestroy<CSACState,CSACOperation>;
    return f;}




// ── RCS (Randomized Counter Sharing) ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> RCSBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    // FIX: defaults now match the Java reference RCS.java (d=4, w=(M/32)/4=8192).
    // The previous l=50, w=655 also fills 1 Mbit but gives ~2.5x the error.
    int32_t l=4,width=8192;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) l=params[0];
    if(params.size()>=2) width=params[1];
    if(l<1||l>512) throw BinderException("rcs_agg: l 1-512");
    if(width<8||width>(1<<24)) throw BinderException("rcs_agg: width 8-16M");
    return make_uniq<RCSBindData>(l,width);}

AggregateFunction GetRCSAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<RCSState,string_t,string_t,RCSOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="rcs_agg"; f.bind=RCSBind;
    f.destructor=AggregateFunction::StateDestroy<RCSState,RCSOperation>;
    return f;}




// ── RCS (Randomized Counter Sharing)AC ═══════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> RCSACBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t l=512,width=131072;
    auto params=BindFlowKeyAggregate(ctx,fn,args,2);   // key columns -> one VARCHAR key
    if(params.size()>=1) l=params[0];
    if(params.size()>=2) width=params[1];
    if(l<1||l>512) throw BinderException("rcsac_agg: l 1-512");
    if(width<8||width>(1<<24)) throw BinderException("rcsac_agg: width 8-16M");
    return make_uniq<RCSACBindData>(l,width);}

AggregateFunction GetRCSACAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<RCSACState,string_t,string_t,RCSACOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="rcsac_agg"; f.bind=RCSACBind;
    f.destructor=AggregateFunction::StateDestroy<RCSACState,RCSACOperation>;
    return f;}




// ── SSVS-2 (Single Update Sketch with Variable Counter Structure) ════════════════════════════════════════════════════════════════════
static unique_ptr<FunctionData> SSVSBind(
    ClientContext&ctx,AggregateFunction&fn,vector<unique_ptr<Expression>>&args){
    int32_t l=4,m=58254,k=4;  // m = 1048576/18 = 58254
    auto params=BindFlowKeyAggregate(ctx,fn,args,3);   // key columns -> one VARCHAR key
    if(params.size()>=1) l=params[0];
    if(params.size()>=2) m=params[1];
    if(params.size()>=3) k=params[2];
    if(l<1||l>64) throw BinderException("ssvs_agg: l 1-64");
    if(m<8||m>(1<<24)) throw BinderException("ssvs_agg: m 8-16M");
    if(k<1) throw BinderException("ssvs_agg: k must be >= 1");
    return make_uniq<SSVSBindData>(l,m,k);}

AggregateFunction GetSSVSAggregateFunction(){
    auto f=AggregateFunction::UnaryAggregate<SSVSState,string_t,string_t,SSVSOperation>(
        LogicalType::VARCHAR,LogicalType::BLOB);
    f.name="ssvs_agg"; f.bind=SSVSBind;
    f.destructor=AggregateFunction::StateDestroy<SSVSState,SSVSOperation>;
    return f;}





// ═════════════════════════════════════════════════════════════════════════════
// Timed insert (update) functions:  xxx_update_timed(sketch, key...)
// ─────────────────────────────────────────────────────────────────────────────
// Measures the time to add one packet to a sketch: hash the flow key and
// update the counters, exactly the per-row work xxx_agg does (including the
// per-row key hashes of the multi-update sketches, see FlowKey in the header).
//
// Method
//   * One flow is inserted TIMED_INSERTS (100,000) times per timed run, and the
//     elapsed time is divided by 100,000. A single insert takes only
//     nanoseconds, far below what one timer start/stop can resolve, so the
//     loop spreads the timer's own cost (~20-50 ns, plus its ~42 ns tick on
//     Apple Silicon) across 100,000 inserts, making it negligible.
//   * steady_clock (monotonic). high_resolution_clock is the wall clock on
//     Linux and can jump when the system time is adjusted.
//   * The key is re-hashed on every insert (and on every row, for the
//     multi-update sketches). Its pointer is read through a volatile, so the
//     compiler cannot hash once and reuse the result.
//   * After the loop the sketch is queried and the answer stored in a
//     volatile, so the compiler cannot delete the inserts as unused work.
//   * TIMED_RUNS (5) runs, each on a fresh copy of the sketch (copying is not
//     timed). insert_ns is the median, which ignores a run that was slowed by
//     an interrupt or by landing on an efficiency core. insert_ns_min is the
//     fastest run.
//   * hash_ns times ONE key hash on its own, the same way. hashes_per_insert
//     is how many key hashes one insert does (counted in an extra, untimed
//     run): 1 for RCS, RCS-AC and SSVS, d for CM, CU, CS and the SC/AC
//     versions, and d plus the rows re-hashed at the minimum for CU-SC/CU-AC.
//     insert_ns - hash_ns * hashes_per_insert is the counter-update cost alone.
//
// Returns STRUCT(sketch BLOB, insert_ns DOUBLE, insert_ns_min DOUBLE,
//                inserts_per_run INTEGER, hash_ns DOUBLE, hashes_per_insert DOUBLE).
// `sketch` is the input sketch, unchanged: the timing runs on copies only.
//
// Note: inserting the SAME flow repeatedly keeps its counters in the CPU
// cache, so this measures the best-case (cache-hot) insert cost. Sketches with
// probabilistic counters (AC variants, SSVS once a counter grows) also skip
// more updates as the counter grows during the 100,000 inserts. For times
// comparable to the paper's, use the whole-trace xxx_agg_timed() aggregates.
// ═════════════════════════════════════════════════════════════════════════════
static constexpr int TIMED_INSERTS = 100000;  // inserts per timed run
static constexpr int TIMED_RUNS    = 5;       // timed runs; the median is reported

static duckdb::LogicalType TimedUpdateReturnType(){
    duckdb::child_list_t<duckdb::LogicalType> fields;
    fields.emplace_back("sketch",            duckdb::LogicalType::BLOB);     // input sketch, unchanged
    fields.emplace_back("insert_ns",         duckdb::LogicalType::DOUBLE);   // median ns per insert
    fields.emplace_back("insert_ns_min",     duckdb::LogicalType::DOUBLE);   // fastest run, ns per insert
    fields.emplace_back("inserts_per_run",   duckdb::LogicalType::INTEGER);  // 100,000
    fields.emplace_back("hash_ns",           duckdb::LogicalType::DOUBLE);   // median ns for ONE key hash
    fields.emplace_back("hashes_per_insert", duckdb::LogicalType::DOUBLE);   // key hashes per insert
    return duckdb::LogicalType::STRUCT(std::move(fields));
}

// One insert. RCS, RCS-AC and SSVS also take the random state that picks the row.
template<class SK, class K> static inline void InsertOnce(SK &sk, const K &key, uint64_t &) { sk.Insert(key); }
template<class K> static inline void InsertOnce(RCSSketch &sk, const K &key, uint64_t &rng)   { sk.Insert(key, rng); }
template<class K> static inline void InsertOnce(RCSACSketch &sk, const K &key, uint64_t &rng) { sk.Insert(key, rng); }
template<class K> static inline void InsertOnce(SSVSSketch &sk, const K &key, uint64_t &rng)  { sk.Insert(key, rng); }

// One timed run: TIMED_INSERTS inserts of `key` into a fresh copy of `base`.
// Returns nanoseconds per insert.
template<class SK>
static double TimeOneRun(const SK &base, const string_t &key){
    SK work = base;                                        // fresh copy (not timed)
    uint64_t rng = RandomSeed();
    const char *const volatile keyData = key.GetData();   // re-read every insert
    const size_t keySize = key.GetSize();

    auto t0 = std::chrono::steady_clock::now();
    for(int r = 0; r < TIMED_INSERTS; r++)
        InsertOnce(work, FlowKey(keyData, keySize), rng);
    auto t1 = std::chrono::steady_clock::now();

    // Read the sketch so the inserts above have an observable effect.
    volatile int64_t sink = work.Query(FNVHash1(key.GetData(), keySize));
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / TIMED_INSERTS;
}

// Untimed: the average number of key hashes one insert does, over a run of the
// same length as a timed run (for CU-SC/CU-AC it depends on the counters).
template<class SK>
static double HashesPerInsert(const SK &base, const string_t &key){
#if SKETCHES_HASH_PER_ROW
    SK work = base;
    uint64_t rng = RandomSeed();
    int64_t calls = 0;
    FlowKey flowKey(key.GetData(), key.GetSize());
    CountingKey<FlowKey> counted{flowKey, &calls};
    for(int r = 0; r < TIMED_INSERTS; r++)
        InsertOnce(work, counted, rng);
    return (double)calls / TIMED_INSERTS;
#else
    return 1.0;   // built with SKETCHES_HASH_PER_ROW=0: one hash per insert
#endif
}

// One timed run of the key hash alone (same loop, no sketch update).
static double TimeHashOnly(const string_t &key){
    const char *const volatile keyData = key.GetData();
    const size_t keySize = key.GetSize();
    uint32_t acc = 0;
    auto t0 = std::chrono::steady_clock::now();
    for(int r = 0; r < TIMED_INSERTS; r++)
        acc += (uint32_t)FNVHash1(keyData, keySize);
    auto t1 = std::chrono::steady_clock::now();
    volatile uint32_t sink = acc;   // keep the hashes from being optimized away
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / TIMED_INSERTS;
}

template<class SK>
static void TimedUpdateImpl(DataChunk &args, ExpressionState &, Vector &result){
    auto count = args.size();
    UnifiedVectorFormat sd, kd;
    args.data[0].ToUnifiedFormat(count, sd);
    args.data[1].ToUnifiedFormat(count, kd);
    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto &children = StructVector::GetEntries(result);
    auto *sketchOut = FlatVector::GetData<string_t>(*children[0]);
    auto *medianNs  = FlatVector::GetData<double>(*children[1]);
    auto *minNs     = FlatVector::GetData<double>(*children[2]);
    auto *perRun    = FlatVector::GetData<int32_t>(*children[3]);
    auto *hashNs    = FlatVector::GetData<double>(*children[4]);
    auto *hashesPer = FlatVector::GetData<double>(*children[5]);
    for(idx_t i = 0; i < count; i++){
        auto si = sd.sel->get_index(i), ki = kd.sel->get_index(i);
        if(!sd.validity.RowIsValid(si) || !kd.validity.RowIsValid(ki)){
            FlatVector::SetNull(result, i, true);          // NULL struct (and its fields)
            continue;
        }
        string_t blob = ((string_t *)sd.data)[si];
        string_t key  = ((string_t *)kd.data)[ki];
        sketchOut[i] = StringVector::AddStringOrBlob(*children[0], blob.GetData(), blob.GetSize());

        SK base = SK::Deserialize(blob.GetData(), blob.GetSize());
        double runs[TIMED_RUNS];
        for(int run = 0; run < TIMED_RUNS; run++) runs[run] = TimeOneRun(base, key);
        std::sort(runs, runs + TIMED_RUNS);
        medianNs[i] = runs[TIMED_RUNS / 2];
        minNs[i]    = runs[0];
        perRun[i]   = TIMED_INSERTS;

        double hashRuns[TIMED_RUNS];
        for(int run = 0; run < TIMED_RUNS; run++) hashRuns[run] = TimeHashOnly(key);
        std::sort(hashRuns, hashRuns + TIMED_RUNS);
        hashNs[i]    = hashRuns[TIMED_RUNS / 2];
        hashesPer[i] = HashesPerInsert(base, key);
    }
}

static ScalarFunction MakeTimedUpdate(const char *name, scalar_function_t impl){
    return ScalarFunction(name, {LogicalType::BLOB, LogicalType::VARCHAR}, TimedUpdateReturnType(), impl);
}
ScalarFunction GetCountMinTimedUpdateFunction()   { return MakeTimedUpdate("countmin_update_timed",    TimedUpdateImpl<CountMinSketch>); }
ScalarFunction GetCUTimedUpdateFunction()         { return MakeTimedUpdate("cu_update_timed",          TimedUpdateImpl<CountMinSketch>); }
ScalarFunction GetCountSketchTimedUpdateFunction(){ return MakeTimedUpdate("countsketch_update_timed", TimedUpdateImpl<CountSketch>); }
ScalarFunction GetCMSCTimedUpdateFunction()       { return MakeTimedUpdate("cmsc_update_timed",        TimedUpdateImpl<CMSCSketch>); }
ScalarFunction GetCUSCTimedUpdateFunction()       { return MakeTimedUpdate("cusc_update_timed",        TimedUpdateImpl<CMSCSketch>); }
ScalarFunction GetCSSCTimedUpdateFunction()       { return MakeTimedUpdate("cssc_update_timed",        TimedUpdateImpl<CSSCSketch>); }
ScalarFunction GetCMACTimedUpdateFunction()       { return MakeTimedUpdate("cmac_update_timed",        TimedUpdateImpl<CMACSketch>); }
ScalarFunction GetCUACTimedUpdateFunction()       { return MakeTimedUpdate("cuac_update_timed",        TimedUpdateImpl<CMACSketch>); }
ScalarFunction GetCSACTimedUpdateFunction()       { return MakeTimedUpdate("csac_update_timed",        TimedUpdateImpl<CSACSketch>); }
ScalarFunction GetRCSTimedUpdateFunction()        { return MakeTimedUpdate("rcs_update_timed",         TimedUpdateImpl<RCSSketch>); }
ScalarFunction GetRCSACTimedUpdateFunction()      { return MakeTimedUpdate("rcsac_update_timed",       TimedUpdateImpl<RCSACSketch>); }
ScalarFunction GetSSVSTimedUpdateFunction()       { return MakeTimedUpdate("ssvs_update_timed",        TimedUpdateImpl<SSVSSketch>); }


// ═════════════════════════════════════════════════════════════════════════════
// Timed query functions:  xxx_query_timed(sketch, key...)
// ─────────────────────────────────────────────────────────────────────────────
// Measures the time to estimate one flow's count, using the same method as the
// timed inserts above: hash the key + read the counters, TIMED_QUERIES (10,000)
// times per run, steady_clock, TIMED_RUNS (5) runs, median reported. The
// answers are summed into a local and stored to a volatile once after the
// loop, so the queries can't be optimized away and no volatile access is timed.
// One untimed query first warms the cache (and lets SSVS compute its noise
// estimate if the sketch doesn't carry one).
//
// Returns STRUCT(estimate BIGINT, query_ns DOUBLE, query_ns_min DOUBLE,
//                queries_per_run INTEGER, hash_ns DOUBLE).
// estimate is the normal xxx_query() answer. hash_ns times the key hash alone,
// so query_ns - hash_ns is the counter-lookup cost.
// ═════════════════════════════════════════════════════════════════════════════
static constexpr int TIMED_QUERIES = 10000;   // queries per timed run

static duckdb::LogicalType TimedQueryReturnType(){
    duckdb::child_list_t<duckdb::LogicalType> fields;
    fields.emplace_back("estimate",        duckdb::LogicalType::BIGINT);   // xxx_query() answer
    fields.emplace_back("query_ns",        duckdb::LogicalType::DOUBLE);   // median ns per query
    fields.emplace_back("query_ns_min",    duckdb::LogicalType::DOUBLE);   // fastest run
    fields.emplace_back("queries_per_run", duckdb::LogicalType::INTEGER);  // 10,000
    fields.emplace_back("hash_ns",         duckdb::LogicalType::DOUBLE);   // median ns to hash the key alone
    return duckdb::LogicalType::STRUCT(std::move(fields));
}

template<class SK>
static double TimeQueryRun(SK &sk, const string_t &key){
    const char *const volatile keyData = key.GetData();   // re-read every query
    const size_t keySize = key.GetSize();
    int64_t acc = 0;
    auto t0 = std::chrono::steady_clock::now();
    for(int r = 0; r < TIMED_QUERIES; r++)
        acc += sk.Query(FNVHash1(keyData, keySize));
    auto t1 = std::chrono::steady_clock::now();
    volatile int64_t sink = acc;   // the answers are used, so the loop must run
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / TIMED_QUERIES;
}

template<class SK>
static void TimedQueryImpl(DataChunk &args, ExpressionState &, Vector &result){
    auto count = args.size();
    UnifiedVectorFormat sd, kd;
    args.data[0].ToUnifiedFormat(count, sd);
    args.data[1].ToUnifiedFormat(count, kd);
    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto &children = StructVector::GetEntries(result);
    auto *estimate = FlatVector::GetData<int64_t>(*children[0]);
    auto *medianNs = FlatVector::GetData<double>(*children[1]);
    auto *minNs    = FlatVector::GetData<double>(*children[2]);
    auto *perRun   = FlatVector::GetData<int32_t>(*children[3]);
    auto *hashNs   = FlatVector::GetData<double>(*children[4]);
    for(idx_t i = 0; i < count; i++){
        auto si = sd.sel->get_index(i), ki = kd.sel->get_index(i);
        if(!sd.validity.RowIsValid(si) || !kd.validity.RowIsValid(ki)){
            FlatVector::SetNull(result, i, true);
            continue;
        }
        string_t blob = ((string_t *)sd.data)[si];
        string_t key  = ((string_t *)kd.data)[ki];
        SK sk = SK::Deserialize(blob.GetData(), blob.GetSize());
        estimate[i] = sk.Query(FNVHash1(key.GetData(), key.GetSize()));   // untimed warm-up + the answer

        double runs[TIMED_RUNS], hashRuns[TIMED_RUNS];
        for(int run = 0; run < TIMED_RUNS; run++) runs[run] = TimeQueryRun(sk, key);
        for(int run = 0; run < TIMED_RUNS; run++) hashRuns[run] = TimeHashOnly(key);
        std::sort(runs, runs + TIMED_RUNS);
        std::sort(hashRuns, hashRuns + TIMED_RUNS);
        medianNs[i] = runs[TIMED_RUNS / 2];
        minNs[i]    = runs[0];
        perRun[i]   = TIMED_QUERIES;
        hashNs[i]   = hashRuns[TIMED_RUNS / 2];
    }
}

static ScalarFunction MakeTimedQuery(const char *name, scalar_function_t impl){
    return ScalarFunction(name, {LogicalType::BLOB, LogicalType::VARCHAR}, TimedQueryReturnType(), impl);
}
ScalarFunction GetCountMinTimedQueryFunction()   { return MakeTimedQuery("countmin_query_timed",    TimedQueryImpl<CountMinSketch>); }
ScalarFunction GetCUTimedQueryFunction()         { return MakeTimedQuery("cu_query_timed",          TimedQueryImpl<CountMinSketch>); }
ScalarFunction GetCountSketchTimedQueryFunction(){ return MakeTimedQuery("countsketch_query_timed", TimedQueryImpl<CountSketch>); }
ScalarFunction GetCMSCTimedQueryFunction()       { return MakeTimedQuery("cmsc_query_timed",        TimedQueryImpl<CMSCSketch>); }
ScalarFunction GetCUSCTimedQueryFunction()       { return MakeTimedQuery("cusc_query_timed",        TimedQueryImpl<CMSCSketch>); }
ScalarFunction GetCSSCTimedQueryFunction()       { return MakeTimedQuery("cssc_query_timed",        TimedQueryImpl<CSSCSketch>); }
ScalarFunction GetCMACTimedQueryFunction()       { return MakeTimedQuery("cmac_query_timed",        TimedQueryImpl<CMACSketch>); }
ScalarFunction GetCUACTimedQueryFunction()       { return MakeTimedQuery("cuac_query_timed",        TimedQueryImpl<CMACSketch>); }
ScalarFunction GetCSACTimedQueryFunction()       { return MakeTimedQuery("csac_query_timed",        TimedQueryImpl<CSACSketch>); }
ScalarFunction GetRCSTimedQueryFunction()        { return MakeTimedQuery("rcs_query_timed",         TimedQueryImpl<RCSSketch>); }
ScalarFunction GetRCSACTimedQueryFunction()      { return MakeTimedQuery("rcsac_query_timed",       TimedQueryImpl<RCSACSketch>); }
ScalarFunction GetSSVSTimedQueryFunction()       { return MakeTimedQuery("ssvs_query_timed",        TimedQueryImpl<SSVSSketch>); }


// ═════════════════════════════════════════════════════════════════════════════
// Whole-trace timed aggregates:  xxx_agg_timed(key... [, sizing constants])
// ─────────────────────────────────────────────────────────────────────────────
// Builds exactly the same sketch as xxx_agg(...) (same arguments, same sizing
// constants) and also measures how long the inserts took. This is how the
// paper measured speed (Table 13): every packet of the trace inserted once,
// in trace order, into one sketch, total time divided by the number of packets.
// Big and small flows, cache misses, counter promotions and the probabilistic
// counters all count in proportion to how often they happen in the real trace.
//
// DuckDB hands an aggregate its rows in chunks of up to 2,048. The timer runs
// around each chunk's inserts only, so reading the table and joining the key
// columns into one string are NOT timed, just like the Java code, which reads
// the whole trace into an array of flow IDs before starting its timer. (Two
// clock reads per 2,048 packets add about 0.02 ns per packet.)
//
// For numbers comparable to the paper, run it on one thread so the trace is
// inserted in order into a single sketch:   SET threads = 1;
// With more threads each thread times its own share and the times are added,
// so insert_ns is still nanoseconds of CPU work per packet.
// With GROUP BY, each chunk's time is split evenly over its rows.
//
// Returns STRUCT(sketch BLOB, insert_ns DOUBLE, packets BIGINT, total_ms DOUBLE)
//   sketch     the sketch xxx_agg() would build
//   insert_ns  nanoseconds per packet (total time / packets)
//   packets    packets inserted (rows with a non-NULL key)
//   total_ms   total insert time in milliseconds
// ═════════════════════════════════════════════════════════════════════════════
static LogicalType TraceTimedReturnType(){
    child_list_t<LogicalType> fields;
    fields.emplace_back("sketch",    LogicalType::BLOB);     // same sketch as xxx_agg()
    fields.emplace_back("insert_ns", LogicalType::DOUBLE);   // ns per packet
    fields.emplace_back("packets",   LogicalType::BIGINT);   // packets inserted
    fields.emplace_back("total_ms",  LogicalType::DOUBLE);   // total insert time
    return LogicalType::STRUCT(std::move(fields));
}

// The sketch's normal aggregate state plus the time spent and packets inserted.
template<class STATE>
struct TraceTimedState : STATE {
    double  timed_ns;
    int64_t packets;
};

// Forwards every callback to the sketch's own Operation and keeps the counts.
template<class OP>
struct TraceTimedOperation {
    template<class S> static void Initialize(S &s){
        OP::Initialize(s);
        s.timed_ns = 0;
        s.packets  = 0;
    }
    template<class I, class S, class OP2>
    static void Operation(S &s, const I &in, AggregateUnaryInput &u){
        OP::template Operation<I, S, OP>(s, in, u);
        s.packets++;
    }
    template<class I, class S, class OP2>
    static void ConstantOperation(S &s, const I &in, AggregateUnaryInput &u, idx_t n){
        OP::template ConstantOperation<I, S, OP>(s, in, u, n);
        s.packets += (int64_t)n;
    }
    template<class S, class OP2>
    static void Combine(const S &src, S &tgt, AggregateInputData &a){
        OP::template Combine<S, OP>(src, tgt, a);
        tgt.timed_ns += src.timed_ns;
        tgt.packets  += src.packets;
    }
    template<class T, class S>
    static void Finalize(S &s, T &target, AggregateFinalizeData &fd){
        OP::template Finalize<T, S>(s, target, fd);
    }
    template<class S> static void Destroy(S &s, AggregateInputData &a){ OP::Destroy(s, a); }
    static bool IgnoreNull(){ return true; }
};

template<class STATE, class OP>
struct TraceTimedAggregate {
    using TS  = TraceTimedState<STATE>;
    using TOP = TraceTimedOperation<OP>;

    static double ElapsedNs(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1){
        return std::chrono::duration<double, std::nano>(t1 - t0).count();
    }

    // Ungrouped aggregate (the usual SELECT xxx_agg_timed(...) FROM t): one state per chunk.
    static void SimpleUpdate(Vector inputs[], AggregateInputData &aid, idx_t input_count,
                             data_ptr_t state, idx_t count){
        auto t0 = std::chrono::steady_clock::now();
        AggregateFunction::UnaryUpdate<TS, string_t, TOP>(inputs, aid, input_count, state, count);
        auto t1 = std::chrono::steady_clock::now();
        reinterpret_cast<TS *>(state)->timed_ns += ElapsedNs(t0, t1);
    }

    // GROUP BY: rows of one chunk can go to different groups, so the chunk's
    // time is shared out evenly over its rows.
    static void ScatterUpdate(Vector inputs[], AggregateInputData &aid, idx_t input_count,
                              Vector &states, idx_t count){
        auto t0 = std::chrono::steady_clock::now();
        AggregateFunction::UnaryScatterUpdate<TS, string_t, TOP>(inputs, aid, input_count, states, count);
        auto t1 = std::chrono::steady_clock::now();
        if (count == 0) return;
        double share = ElapsedNs(t0, t1) / (double)count;
        UnifiedVectorFormat sdata;
        states.ToUnifiedFormat(count, sdata);
        auto statePtrs = UnifiedVectorFormat::GetData<TS *>(sdata);
        for (idx_t i = 0; i < count; i++) statePtrs[sdata.sel->get_index(i)]->timed_ns += share;
    }

    static void WriteTiming(const TS &s, vector<unique_ptr<Vector>> &children, idx_t row, bool constant){
        auto &nsVec = *children[1], &packetsVec = *children[2], &msVec = *children[3];
        double *ns  = constant ? ConstantVector::GetData<double>(nsVec)       : FlatVector::GetData<double>(nsVec);
        int64_t *pk = constant ? ConstantVector::GetData<int64_t>(packetsVec) : FlatVector::GetData<int64_t>(packetsVec);
        double *ms  = constant ? ConstantVector::GetData<double>(msVec)       : FlatVector::GetData<double>(msVec);
        pk[row] = s.packets;
        ms[row] = s.timed_ns / 1e6;
        if (s.packets > 0) {
            ns[row] = s.timed_ns / (double)s.packets;
        } else if (constant) {
            ConstantVector::SetNull(nsVec, true);
        } else {
            FlatVector::SetNull(nsVec, row, true);
        }
    }

    static void Finalize(Vector &states, AggregateInputData &aid, Vector &result, idx_t count, idx_t offset){
        auto &children = StructVector::GetEntries(result);
        if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);   // the struct's fields too
            AggregateExecutor::Finalize<TS, string_t, TOP>(states, aid, *children[0], count, offset);
            WriteTiming(**ConstantVector::GetData<TS *>(states), children, 0, true);
        } else {
            result.SetVectorType(VectorType::FLAT_VECTOR);
            AggregateExecutor::Finalize<TS, string_t, TOP>(states, aid, *children[0], count, offset);
            auto statePtrs = FlatVector::GetData<TS *>(states);
            for (idx_t i = 0; i < count; i++) WriteTiming(*statePtrs[i], children, i + offset, false);
        }
    }

    // `plain` is the sketch's normal xxx_agg function; its bind (arguments,
    // sizing constants, seeds) is reused so both build identical sketches.
    static AggregateFunction Make(const AggregateFunction &plain, const char *name){
        auto f = AggregateFunction::UnaryAggregate<TS, string_t, string_t, TOP>(
            LogicalType::VARCHAR, TraceTimedReturnType());
        f.name          = name;
        f.bind          = plain.bind;
        f.destructor    = AggregateFunction::StateDestroy<TS, TOP>;
        f.update        = ScatterUpdate;
        f.simple_update = SimpleUpdate;
        f.finalize      = Finalize;
        return f;
    }
};

AggregateFunction GetCountMinTimedAggregateFunction()   { return TraceTimedAggregate<CountMinState,    CountMinOperation   >::Make(GetCountMinAggregateFunction(),    "countmin_agg_timed"); }
AggregateFunction GetCUTimedAggregateFunction()         { return TraceTimedAggregate<CountMinState,    CountMinOperation   >::Make(GetCUAggregateFunction(),          "cu_agg_timed"); }
AggregateFunction GetCountSketchTimedAggregateFunction(){ return TraceTimedAggregate<CountSketchState, CountSketchOperation>::Make(GetCountSketchAggregateFunction(), "countsketch_agg_timed"); }
AggregateFunction GetCMSCTimedAggregateFunction()       { return TraceTimedAggregate<CMSCState,        CMSCOperation       >::Make(GetCMSCAggregateFunction(),        "cmsc_agg_timed"); }
AggregateFunction GetCUSCTimedAggregateFunction()       { return TraceTimedAggregate<CMSCState,        CMSCOperation       >::Make(GetCUSCAggregateFunction(),        "cusc_agg_timed"); }
AggregateFunction GetCSSCTimedAggregateFunction()       { return TraceTimedAggregate<CSSCState,        CSSCOperation       >::Make(GetCSSCAggregateFunction(),        "cssc_agg_timed"); }
AggregateFunction GetCMACTimedAggregateFunction()       { return TraceTimedAggregate<CMACState,        CMACOperation       >::Make(GetCMACAggregateFunction(),        "cmac_agg_timed"); }
AggregateFunction GetCUACTimedAggregateFunction()       { return TraceTimedAggregate<CMACState,        CMACOperation       >::Make(GetCUACAggregateFunction(),        "cuac_agg_timed"); }
AggregateFunction GetCSACTimedAggregateFunction()       { return TraceTimedAggregate<CSACState,        CSACOperation       >::Make(GetCSACAggregateFunction(),        "csac_agg_timed"); }
AggregateFunction GetRCSTimedAggregateFunction()        { return TraceTimedAggregate<RCSState,         RCSOperation        >::Make(GetRCSAggregateFunction(),         "rcs_agg_timed"); }
AggregateFunction GetRCSACTimedAggregateFunction()      { return TraceTimedAggregate<RCSACState,       RCSACOperation      >::Make(GetRCSACAggregateFunction(),       "rcsac_agg_timed"); }
AggregateFunction GetSSVSTimedAggregateFunction()       { return TraceTimedAggregate<SSVSState,        SSVSOperation       >::Make(GetSSVSAggregateFunction(),        "ssvs_agg_timed"); }

} // namespace duckdb_sketches
