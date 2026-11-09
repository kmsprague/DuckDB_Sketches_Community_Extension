// sketches_extension.hpp
// ----------------------
// Declares the SketchesExtension class which is the entry point for the DuckDB
// extension. DuckDB calls Load() when the extension is loaded, and Name() /
// Version() to identify it in the catalog.
//
// This extension implements the four baseline sketch structures from the paper:
// "Single Update Sketch with Variable Counter Structure" (VLDB 2023)
//
//   CM  - Count-Min Sketch         (multi-update, minimum query)
//   CU  - Counter Update Sketch    (multi-update, minimum query, conservative)
//   CS  - Count Sketch             (multi-update, median query, signed counters)
//   RCS - Randomized Counter Sharing (single-update, sum minus noise query)

#pragma once
#include "duckdb.hpp"

namespace duckdb_sketches{

class SketchesExtension : public duckdb::Extension{

    public:
        // Called by DuckDB when the extension is loaded
        // All the functions also get loaded into the database here
        void Load(duckdb::DuckDB &db) override;

        // Returns the extension name 
        std::string Name() override;

        // Returns the extension version
        std::string Version() const override;
};

} // namespace duckdb_sketches

// Expose the class in the global namespace so DuckDB's auto-generated
// extension loader (codegen/src/generated_extension_loader.cpp) can find it
// without needing to know our namespace.
using SketchesExtension = duckdb_sketches::SketchesExtension;