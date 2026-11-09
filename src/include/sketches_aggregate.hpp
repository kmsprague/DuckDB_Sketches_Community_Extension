// sketches_aggregate.hpp
// ──────────────────────
// Defines all sketch data structures and their DuckDB integration layer.
//
// Sketches implemented (all matching Java reference at https://github.com/DimitrisMel/SSVS):
//   CM   — Count-Min Sketch               (basic frequency estimator, min of d rows)
//   CU   — Counter Update (conservative)  (CM variant that only increments minimums)
//   CS   — Count Sketch                   (signed counters, median query, less bias)
//   CM-SC / CU-SC / CS-SC — SC variants   (Self-Adjusting Counters pack 4 sub-slots)
//   CM-AC / CU-AC / CS-AC — AC variants   (Active Counters use probabilistic updates)
//   RCS  — Randomized Counter Sharing     (single-update, 2D d×w array)
//   RCS-AC — RCS with Active Counters     (flat 1D array of 8-bit AC counters)
//   SSVS-2 — Variable Counter Structure  (paper's main contribution, best accuracy)
//
// Memory budget: all sketches target 1 Mbit (1,048,576 bits = 131,072 bytes).
// Default widths are derived from M_BITS / counterSize / depth.
//
// Hash pipeline (matching Java exactly):
//   1. FNVHash1(string) → int32_t base hash
//   2. intHash(base ^ seed[row]) → per-row column index
//   The seed per row is stored in BindData and passed to each sketch at construction.
//   Like the Java code, the multi-update sketches redo step 1 for every row
//   (see FlowKey below); SKETCHES_HASH_PER_ROW=0 turns that off.
//
// DuckDB integration:
//   Each sketch has a matching XxxState (per-thread agg state), XxxBindData
//   (query-time parameters incl. seeds), and XxxOperation (agg callbacks).
//   Scalar query/update/merge functions operate on serialized BLOBs.
#pragma once
#include "duckdb.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/exception.hpp"   // InvalidInputException (merge checks)
#include <vector>     // std::vector for counter tables and seed arrays
#include <algorithm>  // std::sort for median in CountSketch / SSVS query
#include <cstdint>    // int32_t, uint64_t, etc.
#include <cstring>    // memcpy for serialization
#include <cmath>      // std::abs for counter magnitude helpers
#include <random>     // std::mt19937_64 for seed generation

namespace duckdb_sketches {

// ─── Utility functions ────────────────────────────────────────────────────────

// Returns a random 64 bit int to be used as a seed for hasing
static inline uint64_t RandomSeed(){
    thread_local std::mt19937_64 g([]{
        std::random_device rd;          // hardware entropy (e.g. /dev/urandom)
        return ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    }());
    return g();
}

// FIX: pick a row/seed index from the HIGH bits of the LCG state.
// The low k bits of a power-of-two LCG have period 2^k, so `rng % 4` walks the
// fixed cycle 2,1,0,3,2,1,0,3,... Any flow whose packets arrive at a regular
// spacing (e.g. two flows alternating A,B,A,B) then lands in the same rows every
// time; with SSVS's min-gap query that made such a flow read 2x too high while
// its partner read ~0. The high bits don't have this problem.
static inline int32_t LcgPick(uint64_t &rng, int32_t n){
    rng = rng*6364136223846793005ULL + 1442695040888963407ULL;
    return (int32_t)((rng >> 33) % (uint64_t)n);
}

// FIX: fixed per-row hash seeds (splitmix64 of the row number).
// Previously every xxx_agg() call drew fresh random seeds, so two sketches built
// in separate queries (one per file, per day, per half of a table...) hashed the
// same flow to unrelated counters, and xxx_merge() silently added mismatched
// counters: every flow lost the packets from the other sketch. With fixed seeds,
// every sketch of the same kind and size uses the same hash functions and can
// be merged. (Only the hash seeds are fixed; the random choices made by RCS,
// SSVS and the active counters still use RandomSeed().)
static inline uint64_t RowSeed(int32_t row){
    uint64_t z = 0x5EED5EED5EED5EEDULL + 0x9E3779B97F4A7C15ULL * (uint64_t)(row + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// FIX: merges must refuse sketches that don't line up. Adding counters from a
// sketch with a different size read past the end of the smaller table, and
// adding counters built with different seeds produced wrong counts silently.
static inline void CheckMergeable(int64_t rows1, int64_t cols1, const std::vector<uint64_t> &seeds1,
                                  int64_t rows2, int64_t cols2, const std::vector<uint64_t> &seeds2){
    if(rows1 != rows2 || cols1 != cols2)
        throw duckdb::InvalidInputException(
            "Cannot merge sketches of different sizes (" + std::to_string(rows1) + " x " + std::to_string(cols1) +
            " vs " + std::to_string(rows2) + " x " + std::to_string(cols2) + "). Build both with the same parameters.");
    if(seeds1 != seeds2)
        throw duckdb::InvalidInputException(
            "Cannot merge sketches built with different hash seeds. Sketches created by an older build of this "
            "extension used random seeds; rebuild them with this version.");
}

// FNVHash1 — matches Java GeneralUtil.FNVHash1(String) exactly.
// This is the first step of the two-step hash pipeline used by every sketch.
// Runs FNV-1a over each byte of the input string, then applies an avalanche
// mixing pass to improve distribution. Returns a signed 32-bit integer,
// which may be negative — callers must use unsigned-safe modulo (jmod/jcol).
//
// FIX: all arithmetic is done on uint32_t. Java's int multiply/add/shift wrap
// around silently, but in C++ signed overflow and left-shifting a negative int
// are UNDEFINED BEHAVIOUR, and optimizing compilers exploit it: at -O3, gcc
// returned wrong counts and Apple clang (macOS arm64) sent every key to the same
// counters, so every flow's estimate equalled the whole table. Unsigned
// arithmetic wraps by definition and produces bit-identical results.
static inline int32_t FNVHash1(const char *d, size_t n){
    const uint32_t p = 16777619U;           // FNV prime for 32-bit
    uint32_t h = 2166136261U;               // FNV offset basis
    for(size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)d[i]) * p;       // XOR each byte then multiply by prime
    // Avalanche mixing pass — improves bit distribution beyond raw FNV.
    // Java's `>>` here is an ARITHMETIC (sign-extending) shift, so the two right
    // shifts are done on the signed value; everything else stays unsigned.
    h += h << 13;
    h ^= (uint32_t)((int32_t)h >> 7);
    h += h << 3;
    h ^= (uint32_t)((int32_t)h >> 17);
    h += h << 5;
    return (int32_t)h;
}

// intHash — matches Java GeneralUtil.intHash(int) (Thomas Wang 32-bit integer hash).
// This is the second step of the hash pipeline: intHash(FNVHash1(item) ^ seed[row]).
// The XOR with the row seed makes each row independent so hash collisions in
// different rows are uncorrelated, which is what gives CM its theoretical guarantees.
//
// CRITICAL: Java uses >>> (unsigned / zero-filling right shift). C++ >> on a signed
// int32_t is arithmetic (sign-extending), so for any negative intermediate value
// the two operations produce different results. Without casting to uint32_t first,
// about 86% of all hash outputs are wrong, completely breaking all sketches.
//
// FIX: computed entirely in uint32_t (see FNVHash1 above) — the old signed
// version overflowed and left-shifted negative values, which is undefined
// behaviour in C++. Unsigned >> is exactly Java's >>>.
static inline int32_t intHash(int32_t key){
    uint32_t k = (uint32_t)key;
    k += ~(k << 15);
    k ^= k >> 10;
    k += k << 3;
    k ^= k >> 6;
    k += ~(k << 11);
    k ^= k >> 16;
    return (int32_t)k;
}

// jcol — column index from a hash value using an unsigned right-shift then safe modulo.
// Matches Java (hashV >>> shift) % w with the +w to handle negative results.
// Used by CS, CM-SC, CU-SC, CS-SC, CM-AC, CU-AC, CS-AC, RCS, SSVS.
// The shift removes the low bits that are used for sign/sub-slot selection.
static inline int32_t jcol(int32_t hv, int shift, int32_t w){
    return ((int32_t)((uint32_t)hv >> shift) % w + w) % w;
}

// jmod — safe modulo for full-hash column (no shift), used by CM, CU, RCS-AC.
// Equivalent to (hashV % w + w) % w — ensures non-negative result even if hv < 0.
static inline int32_t jmod(int32_t hv, int32_t w){
    return ((hv % w) + w) % w;
}

// ─── Flow keys: when the key is hashed ────────────────────────────────────────
// The Java reference hashes the flow ID inside every row loop:
//     for (i = 0; i < d; i++) { hashV = intHash(FNVHash1(flowid) ^ S[i]); ... }
// so CM, CU, CS and their SC/AC versions pay d full key hashes per packet, and
// CU-SC / CU-AC hash each row that is at the minimum a second time before
// updating it. RCS, RCS-AC and SSVS hash the key once per packet. That
// difference is a large part of the per-packet times in the paper (Table 13),
// and why the single-update sketches come out fastest there.
//
// With SKETCHES_HASH_PER_ROW=1 (the default) the multi-update sketches here do
// the same: they are handed a FlowKey and call key.hash() wherever the Java
// code calls FNVHash1(flowid). The hash VALUES are the same either way, so the
// sketches are bit-for-bit identical; only the work per packet changes.
// Build with -DSKETCHES_HASH_PER_ROW=0 to hash every key once per packet
// instead (faster for the multi-update sketches, not how the paper measured).
#ifndef SKETCHES_HASH_PER_ROW
#define SKETCHES_HASH_PER_ROW 1
#endif

struct FlowKey {
    const char *data;
    size_t size;
#if SKETCHES_HASH_PER_ROW
    FlowKey(const char *d, size_t n) : data(d), size(n) {}
    int32_t hash() const {
        // Read the pointer through a volatile so the compiler cannot notice that
        // the bytes are the same every time and reuse the previous hash.
        const char *const volatile p = data;
        return FNVHash1(p, size);
    }
#else
    int32_t cached;
    FlowKey(const char *d, size_t n) : data(d), size(n), cached(FNVHash1(d, n)) {}
    int32_t hash() const { return cached; }
#endif
};

// A key whose hash is already known (Insert(int32_t) overloads, tests).
struct HashedKey {
    int32_t h;
    int32_t hash() const { return h; }
};

// Counts calls to hash(). Used (untimed) by xxx_update_timed to report how
// many key hashes one insert does.
template<class K>
struct CountingKey {
    const K &key;
    int64_t *calls;
    int32_t hash() const { (*calls)++; return key.hash(); }
};

// Small per-insert scratch array for CU-style inserts: on the stack for up to
// 64 rows (every sketch the aggregates build), on the heap beyond that. The old
// code allocated a std::vector on every insert.
struct RowScratch {
    int32_t local[64];
    std::vector<int32_t> big;
    int32_t *p;
    explicit RowScratch(int32_t n) {
        if (n <= 64) p = local;
        else { big.resize(n); p = big.data(); }
    }
    int32_t &operator[](int32_t i) { return p[i]; }
};

// ─── SAC64  (Self-Adjusting Counter, 36 bits: 32-bit C + 4-bit I) ─────────────
// Used by CM-SC, CU-SC, and CS-SC variants. Matches Java SAC64.java exactly.
//
// The key idea is that most flows are small, so we start with small counters
// and only promote them when they overflow. This lets us pack 4 counters into
// the space that a plain 32-bit counter would use, giving 4× more counters
// and therefore 4× less collision noise for the same memory budget.
//
// Counter layout inside the 32-bit C field:
//   Stage I=0 (initial): 4 independent 8-bit signed counters
//     Sub-slot 0: bits  0-7  (bit  0 = sign, bits  1-7 = magnitude)
//     Sub-slot 1: bits  8-15 (bit  8 = sign, bits  9-15 = magnitude)
//     Sub-slot 2: bits 16-23 (bit 16 = sign, bits 17-23 = magnitude)
//     Sub-slot 3: bits 24-31 (bit 24 = sign, bits 25-31 = magnitude)
//
//   Stage I bit 0 set: pair (0,1) merged into 16-bit signed short
//     bits  0-15 (bit  0 = sign, bits  1-15 = magnitude, max ±32767)
//   Stage I bit 2 set: pair (2,3) merged into 16-bit signed short
//     bits 16-31 (bit 16 = sign, bits 17-31 = magnitude)
//
//   Stage I bit 1 set: all 4 sub-slots merged into single 32-bit int
//     bits 0-31 (bit 0 = sign, bits 1-31 = magnitude, max ±2^31-1)
//
// I indicator bits:
//   bit 0 (I & 1): pair (0,1) has been promoted to 16-bit short
//   bit 1 (I & 2): all counters merged into full 32-bit int
//   bit 2 (I & 4): pair (2,3) has been promoted to 16-bit short
struct SAC64 {
    uint32_t C = 0;   // packed counter bits — layout depends on I
    uint8_t  I = 0;   // 4-bit indicator: which pairs have been promoted

    // ── Bit-manipulation helpers ──────────────────────────────────────────────
    // gb: get bits [s..e] (inclusive) from value v
    static uint32_t gb(uint32_t v, int s, int e){
        return (v>>s) & ((1u<<(e-s+1))-1); }
    // sb: set bits [s..e] of v to x, leaving all other bits unchanged
    static uint32_t sb(uint32_t v, int s, int e, uint32_t x){
        uint32_t m=((1u<<(e-s+1))-1)<<s; return (v&~m)|((x<<s)&m); }

    // ── Value readers ─────────────────────────────────────────────────────────
    // Read the signed value stored as one of the 4 byte sub-slots (idx 0-3).
    // Bit layout: bit[idx*8]=sign(1=positive), bits[idx*8+1..+7]=magnitude.
    int32_t getByteVal(int idx) const{
        int b=idx*8; int32_t mg=(int32_t)gb(C,b+1,b+7);
        return gb(C,b,b)?mg:-mg; }
    // Read the signed value of a merged pair (pi=0 → pair 0,1;  pi=1 → pair 2,3).
    // Bit layout: bit[pi*16]=sign, bits[pi*16+1..+15]=magnitude.
    int32_t getShortVal(int pi) const{
        int b=pi*16; int32_t mg=(int32_t)gb(C,b+1,b+15);
        return gb(C,b,b)?mg:-mg; }
    // Read the fully-promoted 32-bit integer value.
    // Bit layout: bit 0=sign, bits 1-31=magnitude.
    int32_t getIntVal() const{
        int32_t mg=(int32_t)gb(C,1,31); return (C&1)?mg:-mg; }
    // Dispatch to the right reader based on the I indicator for this sub-slot.
    int32_t getValue(int idx) const{
        if(I&2) return getIntVal();          // fully promoted to 32-bit int
        int pb=(idx/2)*2;                    // indicator bit position for this pair
        if(I&(1<<pb)) return getShortVal(idx/2);  // pair promoted to 16-bit short
        return getByteVal(idx); }            // still individual 8-bit byte counter

    // ── Value writers ─────────────────────────────────────────────────────────
    // These are only called during promotion (overflow), not on every insert.
    void setByteVal(int32_t v, int idx){
        int b=idx*8; C=sb(C,b,b,v>=0?1:0);     // write sign bit
        int32_t mv=std::abs(v);
        for(int i=1;i<=7;i++){C=sb(C,b+i,b+i,mv&1);mv>>=1;} }  // write magnitude bits LSB-first
    void setShortVal(int32_t v, int pi){
        int b=pi*16; C=sb(C,b,b,v>=0?1:0);     // write sign bit
        int32_t mv=std::abs(v);
        for(int i=1;i<=15;i++){C=sb(C,b+i,b+i,mv&1);mv>>=1;} } // write 15 magnitude bits
    void setIntVal(int32_t v){
        C=sb(C,0,0,v>=0?1:0); int32_t mv=std::abs(v);
        for(int i=1;i<=31;i++){C=sb(C,i,i,mv&1);mv>>=1;} }     // write 31 magnitude bits

    // ── Byte counter update ───────────────────────────────────────────────────
    // Increment or decrement a single 8-bit sub-slot using binary carry propagation.
    // If the counter overflows its 7-bit magnitude, promote the pair to a 16-bit short.
    void incByte(int32_t delta, int idx){
        int b=idx*8;                          // bit offset for this sub-slot
        bool pos=gb(C,b,b);                   // current sign: 1=positive, 0=negative
        int partner=(idx/2)*2+(1-idx%2);      // the other sub-slot in our pair (0↔1, 2↔3)
        bool same=(delta==1&&pos)||(delta==-1&&!pos); // delta matches current direction?
        if(same){
            // Same direction: increment magnitude using carry propagation.
            // Find the first 0 bit in the magnitude field and set it to 1.
            int i=b+1;
            while(i<=b+7&&gb(C,i,i)){C=sb(C,i,i,0);i++;}  // clear carry chain
            if(i<=b+7){C=sb(C,i,i,1);}        // set the first available bit
            else{
                // All 7 magnitude bits are 1 — overflow. Promote this pair to a 16-bit short.
                // The short value = partner's current value + (128 or -128 from this slot).
                int32_t merged=getByteVal(partner)+(pos?128:-128);
                setShortVal(merged,idx/2);
                I|=(uint8_t)(1<<((idx/2)*2)); } // mark this pair as promoted in I
        } else{
            // Opposite direction: decrement magnitude (borrow propagation).
            // Find the first 1 bit and clear it; set all prior bits.
            int i=b+1;
            while(i<=b+7&&!gb(C,i,i)){C=sb(C,i,i,1);i++;} // fill borrow chain with 1s
            if(i<=b+7){C=sb(C,i,i,0);}        // clear the first 1-bit found
            else{
                // All magnitude bits are 0 — sign flip and wrap to magnitude=1.
                C=sb(C,b,b,pos?0u:1u); C=sb(C,b+1,b+1,1);
                for(int j=b+2;j<=b+7;j++) C=sb(C,j,j,0); } }
    }
    // ── Short counter update ──────────────────────────────────────────────────
    // Same carry/borrow logic but over 15 magnitude bits.
    // On overflow promotes both pairs to a single 32-bit int.
    void incShort(int32_t delta, int pi){
        int b=pi*16; bool pos=gb(C,b,b);
        bool same=(delta==1&&pos)||(delta==-1&&!pos);
        if(same){
            int i=b+1;
            while(i<=b+15&&gb(C,i,i)){C=sb(C,i,i,0);i++;}  // carry chain
            if(i<=b+15){C=sb(C,i,i,1);}
            else{
                // Short overflowed — promote to full 32-bit int.
                // Java sets the int value to ±32768 (the overflow point).
                I|=2; setIntVal(pos?32768:-32768);}
        } else{
            int i=b+1;
            while(i<=b+15&&!gb(C,i,i)){C=sb(C,i,i,1);i++;} // borrow chain
            if(i<=b+15){C=sb(C,i,i,0);}
            else{
                C=sb(C,b,b,pos?0u:1u); C=sb(C,b+1,b+1,1);
                for(int j=b+2;j<=b+15;j++) C=sb(C,j,j,0); } }
    }
    // ── Int counter update ────────────────────────────────────────────────────
    // Same carry/borrow over 31 bits. On overflow: saturate (do nothing — Java
    // behaviour is to silently wrap, but at this scale saturation is fine).
    void incInt(int32_t delta){
        bool pos=C&1;                         // sign is the lowest bit
        bool same=(delta==1&&pos)||(delta==-1&&!pos);
        if(same){
            int i=1; while(i<=31&&gb(C,i,i)){C=sb(C,i,i,0);i++;}
            if(i<=31) C=sb(C,i,i,1);         // carry propagation over 31 bits
        } else{
            int i=1; while(i<=31&&!gb(C,i,i)){C=sb(C,i,i,1);i++;}
            if(i<=31){C=sb(C,i,i,0);}        // borrow propagation
            else{C=sb(C,0,0,pos?0u:1u);C=sb(C,1,1,1);
                 for(int j=2;j<=31;j++) C=sb(C,j,j,0);} } // wrap on underflow
    }
    // ── Main entry point ─────────────────────────────────────────────────────
    // Dispatch to the right update function based on the current I stage.
    // delta is +1 (increment) or -1 (decrement, used by CS-SC signed counters).
    // idx is the sub-slot selector (0-3) derived from hashV & 3.
    void increaseValue(int32_t delta, int idx){
        if(I&2){incInt(delta);return;}         // fully promoted — update 32-bit int
        int pb=(idx/2)*2;                      // indicator bit position for this pair
        if(I&(1<<pb)){incShort(delta,idx/2);return;}  // pair promoted — update 16-bit short
        incByte(delta,idx); }                  // still at byte stage — update 8-bit counter

    // ── Merge helpers ─────────────────────────────────────────────────────────
    // Replay `amount` unit updates into sub-slot idx.
    void addTo(int64_t amount, int idx){
        int32_t sg = amount > 0 ? 1 : -1;
        for(int64_t x = 0, n = amount < 0 ? -amount : amount; x < n; x++) increaseValue(sg, idx);
    }
    // Current total held by pair pi (0 → sub-slots 0,1; 1 → sub-slots 2,3).
    int32_t pairTotal(int pi) const{
        if(I & (1 << (pi*2))) return getShortVal(pi);
        return getByteVal(pi*2) + getByteVal(pi*2 + 1);
    }
    // Collapse pair pi into one 16-bit short holding the sum of its two bytes —
    // the same thing incByte() does when one byte of the pair overflows.
    void promotePair(int pi){
        if((I & 2) || (I & (1 << (pi*2)))) return;
        setShortVal(pairTotal(pi), pi);
        I |= (uint8_t)(1 << (pi*2));
    }
    // Collapse all four sub-slots into the single 32-bit int stage.
    void promoteInt(){
        if(I & 2) return;
        int32_t v = pairTotal(0) + pairTotal(1);
        I |= 2; setIntVal(v);
    }

    // Merge src's values into this SAC64.
    // FIX: when src has already merged a pair (or all four sub-slots), its value is
    // ONE shared counter for those sub-slots. The old code added it only to
    // sub-slot 0 (or 2) of the target. For CM-SC/CU-SC that happened to work (a
    // promoted value is always >= 128, so replaying it overflows the target byte and
    // promotes it anyway), but for CS-SC's signed counters a promoted pair can hold
    // a small value (e.g. +5), and the target's partner sub-slot never received it.
    // Now the target is promoted to the same stage first, so every sub-slot sees
    // the merged total.
    void mergeFrom(const SAC64 &src){
        if(src.I & 2){
            promoteInt();
            addTo(src.getIntVal(), 0);
            return;
        }
        for(int pi = 0; pi < 2; pi++){
            if(src.I & (1 << (pi*2))){
                promotePair(pi);
                addTo(src.getShortVal(pi), pi*2);
            } else{
                addTo(src.getByteVal(pi*2),     pi*2);
                addTo(src.getByteVal(pi*2 + 1), pi*2 + 1);
            }
        }
    }
    void ser(char*&p) const {memcpy(p,&C,4);p+=4;memcpy(p,&I,1);p+=1;}
    static SAC64 deser(const char*&p){
        SAC64 s; memcpy(&s.C,p,4);p+=4;memcpy(&s.I,p,1);p+=1; return s;}
    static size_t sz(){return 5;}
};


// Active counter is a 16 bit counter bit 0 is a sign bit, bits 1-10 are the base, bits 11-15 are the exponent
// The actual value is ±(value << exponent). 
// When the 10-bit value field overflows (all 10 bits set to 1), the exponent is
// incremented by 1 and the value is set to 2^9 (half of 2^10), preserving magnitude.
// Updates are probabilistic meaning an increment of ±1 is only applied with a probability of
// 1 / 2^exponent. 
struct AC16{

    // Counter bit
    uint16_t C = 0;

    // Reads bits s to e from 16 bit int v
    static uint16_t gb(uint16_t v,int s,int e){

        // e-s+1 gets the index to start
        // (1u - index) -1 gets the mask 
        // v>>s shifts the value to the right by s bits the AND with the mask to get the value
        return (v>>s)&((1u<<(e-s+1))-1);
    }

    // Writes bits s to e of 16 bit int v without changing other bits
    static uint16_t sb(uint16_t v,int s,int e,uint16_t x){

        // 1u<<(e-s+1))-1 gets the mask 
        // <<s shifts the mask to the left by s bits to get the correct position
        uint16_t m=((1u<<(e-s+1))-1)<<s; 

        // ~m inverts the mask to get the bits that are not being changed
        // v&~m clears the bits that are being changed in v
        // (x<<s)&m shifts the new value x to the left by s bits and ANDs it with the mask to get the new bits
        return (v&~m)|((x<<s)&m);
    }

    // Decode the counter to a signed integer: ±(value << exponent).
    // The index parameter is ignored — AC16 has no sub-slots, it's one counter.
    int32_t getValue(int) const{
        int32_t exp=gb(C,11,15), abv=gb(C,1,10);  // extract exponent and value fields
        int32_t v=abv<<exp;                         // actual magnitude = value * 2^exp
        return (C&1)?v:-v; }                        // apply sign bit

    // Deterministic increment/decrement — called only when the probabilistic gate passes.
    // Uses the same carry/borrow propagation over 10 value bits as SAC64 uses over 7.
    // When value overflows 10 bits, increments the exponent instead (increaseExp).
    void incInner(int32_t delta){
        bool pos=C&1;
        bool same=(delta==1&&pos)||(delta==-1&&!pos);
        if(same){
            // Incrementing in the current sign direction: carry propagation over value bits
            int i=1; while(i<=10&&gb(C,i,i)){C=sb(C,i,i,0);i++;}
            if(i<=10){C=sb(C,i,i,1);}          // normal increment
            else{
                // Value field overflowed all 10 bits — promote the exponent.
                // Set value bit 10 to 1 (= 2^9 as the new value after shifting),
                // then carry into the 5-bit exponent field.
                C=sb(C,10,10,1);
                int j=11; while(j<=15&&gb(C,j,j)){C=sb(C,j,j,0);j++;}
                if(j<=15) C=sb(C,j,j,1);}      // increment exponent field
        } else{
            // Decrementing against current direction: borrow propagation
            int i=1; while(i<=10&&!gb(C,i,i)){C=sb(C,i,i,1);i++;}
            if(i<=10){C=sb(C,i,i,0);}
            else{                               // underflow: sign flip + magnitude=1
                C=sb(C,0,0,(uint16_t)(pos?0:1));C=sb(C,1,1,1);
                for(int j=2;j<=10;j++) C=sb(C,j,j,0);} }
    }
    // Probabilistic gate: only call incInner with probability 1/2^exp.
    // Advances the LCG random state and checks if the low bits land on zero.
    void increaseValue(int32_t delta, uint64_t &rng){
        int32_t exp=gb(C,11,15), expexp=1<<exp;    // probability denominator = 2^exp
        // LCG step: advance rng, extract upper 31 bits (>>33), check if it's 0 mod 2^exp
        rng=rng*6364136223846793005ULL+1442695040888963407ULL;
        if((int32_t)((uint32_t)(rng>>33)%expexp)==0) incInner(delta); }

    void ser(char*&p) const {memcpy(p,&C,2);p+=2;}
    static AC16 deser(const char*&p){
        AC16 a; memcpy(&a.C,p,2);p+=2; return a;}
    static size_t sz(){return 2;}
};

// ─── AC8  (Active Counter, 8 bits, unsigned) ─────────────────────────────────
// Used by RCS-AC only. Matches Java ActiveCounter8.java.
//
// 8-bit unsigned counter packed as:
//   bits 0-3 : 4-bit value field
//   bits 4-7 : 4-bit exponent field
//
// Unlike AC16, AC8 is unsigned (counts are always positive).
// The value formula is: (value + 16) * 2^exponent - 16
// This offset encoding means the counter starts at 0 (value=0, exp=0 → (0+16)*1-16=0)
// and small values are counted exactly. The +16/-16 offset shifts the range so
// each exponent level overlaps with the next, reducing discontinuities.
//
// Updates are probabilistic with the same 1/2^exp gate as AC16.
struct AC8 {
    uint8_t C = 0;   // packed: [exp:4][value:4]

    // Decode the counter. The (abv+16)*2^exp - 16 formula is taken directly from
    // Java's ActiveCounter8.getValue() — it provides better small-value accuracy
    // than a plain value*2^exp encoding.
    int32_t getValue(int) const{
        int32_t exp=(C>>4)&0xF, abv=C&0xF;    // upper nibble = exp, lower = value
        return (abv+16)*(1<<exp)-16; }          // offset decode formula from Java

    // Deterministic increment of the 4-bit value field with carry into exponent.
    // Overflow of value nibble → increment exponent nibble.
    void incInner(){
        // Carry propagation over bits 0-3 (value nibble)
        int i=0; while(i<=3&&((C>>i)&1)){C&=~(1<<i);i++;}
        if(i<=3){C|=(1<<i);}               // normal value increment
        else{
            // Value overflowed — carry into exponent nibble (bits 4-7)
            int j=4; while(j<=7&&((C>>j)&1)){C&=~(1<<j);j++;}
            if(j<=7) C|=(1<<j);} }         // increment exponent

    // Probabilistic gate + increment. Only unsigned (delta>0) is supported.
    void increaseValue(int32_t delta, uint64_t &rng){
        if(delta<=0) return;                // AC8 is unsigned, ignore decrements
        int32_t exp=(C>>4)&0xF, expexp=1<<exp;
        rng=rng*6364136223846793005ULL+1442695040888963407ULL;
        if((int32_t)((uint32_t)(rng>>33)%expexp)==0) incInner(); }

    void ser(char*&p) const {memcpy(p,&C,1);p+=1;}
    static AC8 deser(const char*&p){
        AC8 a; memcpy(&a.C,p,1);p+=1; return a;}
    static size_t sz(){return 1;}
};

// ─── VC  (Variable Counter, 18 bits: 16-bit raw + 2-bit I) ───────────────────
// Used exclusively by SSVS-2. Matches Java VariableCounter.java exactly.
//
// Each SSVS cell holds TWO logical counters (byte sub-slots 0 and 1) that start
// as independent 8-bit signed values and progressively promote as needed.
// The key insight: SSVS inserts each packet into only ONE randomly chosen cell,
// using both the sign and the sub-slot index from the hash, so two independent
// signed estimates can share one 18-bit cell.
//
// Promotion stages (tracked by the 2-bit I indicator):
//   I=0: Two independent 8-bit signed byte counters packed into 16 bits
//         Byte 0: raw bits  0-7  (bit  0=sign, bits  1-7=magnitude, range ±127)
//         Byte 1: raw bits  8-15 (bit  8=sign, bits  9-15=magnitude, range ±127)
//
//   I=1: Single 16-bit signed short (both byte counters merged on overflow)
//         raw bits 0-15 (bit 0=sign, bits 1-15=magnitude, range ±32767)
//         When merging: new value = other_byte_value + ±128 (from overflowing byte)
//
//   I=2: Small Active Counter
//         raw bits 0-2=exponent(3), bit 3=sign, bits 4-15=12-bit value
//         Probabilistic: update with prob 1/2^exp. Max exp=7 (prob 1/128).
//         Range: ±(value << exp), up to ±4096 * 128 = ±524288
//
//   I=3: Large Active Counter
//         raw bits 0-4=exponent(5), bit 5=sign, bits 6-15=10-bit value
//         Probabilistic: update with prob 1/2^exp. Max exp=31.
//         Range: ±(value << exp), up to ±512 * 2^31 = astronomically large
struct VC {
    int32_t raw=0;  // 16 bits of counter storage, layout depends on I
    int32_t I=0;    // 2-bit stage indicator: 0=byte pair, 1=short, 2=smallAC, 3=largeAC

    static int32_t gb(int32_t v,int s,int e){
        return (v>>s)&((1<<(e-s+1))-1);}
    static int32_t sb(int32_t v,int s,int e,int32_t x){
        int32_t m=((1<<(e-s+1))-1)<<s; return (v&~m)|((x<<s)&m);}

    int32_t getByteVal(int idx) const{
        int b=idx*8; int32_t mg=gb(raw,b+1,b+7);
        return gb(raw,b,b)?mg:-mg;}
    int32_t getShortVal() const{
        int32_t mg=gb(raw,1,15); return gb(raw,0,0)?mg:-mg;}
    int32_t getSmallACVal() const{
        int32_t exp=gb(raw,0,2),mg=gb(raw,4,15);
        return gb(raw,3,3)?(mg<<exp):-(mg<<exp);}
    int32_t getLargeACVal() const{
        int32_t exp=gb(raw,0,4),mg=gb(raw,6,15);
        return gb(raw,5,5)?(mg<<exp):-(mg<<exp);}
    int32_t getValue(int idx) const{
        if(I==0) return getByteVal(idx);
        if(I==1) return getShortVal();
        if(I==2) return getSmallACVal();
        return getLargeACVal();}

    void incByte(int32_t delta, int idx){
        int b=idx*8; bool pos=gb(raw,b,b);
        bool same=(delta==1&&pos)||(delta==-1&&!pos);
        if(same){
            int i=b+1;
            while(i<=b+7&&gb(raw,i,i)){raw=sb(raw,i,i,0);i++;}
            if(i<=b+7){raw=sb(raw,i,i,1);}
            else{
                int32_t other=getByteVal(1-idx);
                int32_t val=other+(pos?128:-128);
                I=1; raw=0; raw=sb(raw,0,0,val>=0?1:0);
                int32_t mv=std::abs(val);
                for(int j=1;j<=15;j++){raw=sb(raw,j,j,mv&1);mv>>=1;}}
        } else{
            int i=b+1;
            while(i<=b+7&&!gb(raw,i,i)){raw=sb(raw,i,i,1);i++;}
            if(i<=b+7){raw=sb(raw,i,i,0);}
            else{raw=sb(raw,b,b,pos?0:1);raw=sb(raw,b+1,b+1,1);
                 for(int j=b+2;j<=b+7;j++) raw=sb(raw,j,j,0);}}
    }
    void incShort(int32_t delta){
        bool pos=gb(raw,0,0); bool same=(delta==1&&pos)||(delta==-1&&!pos);
        if(same){
            int i=1; while(i<=15&&gb(raw,i,i)){raw=sb(raw,i,i,0);i++;}
            if(i<=15){raw=sb(raw,i,i,1);}
            else{
                I=2; raw=0; raw=sb(raw,0,2,4); raw=sb(raw,3,3,pos?1:0);
                int32_t v=2048;
                for(int j=4;j<=15;j++){raw=sb(raw,j,j,v&1);v>>=1;}}
        } else{
            int i=1; while(i<=15&&!gb(raw,i,i)){raw=sb(raw,i,i,1);i++;}
            if(i<=15){raw=sb(raw,i,i,0);}
            else{raw=sb(raw,0,0,pos?0:1);raw=sb(raw,1,1,1);
                 for(int j=2;j<=15;j++) raw=sb(raw,j,j,0);}}
    }
    void incSmallAC(int32_t delta, uint64_t &rng){
        int32_t exp=gb(raw,0,2), expexp=1<<exp;
        rng=rng*6364136223846793005ULL+1442695040888963407ULL;
        if((int32_t)((uint32_t)(rng>>33)%expexp)!=0) return;
        bool pos=gb(raw,3,3); int32_t v=gb(raw,4,15);
        if(!pos) v=-v; v+=delta; int32_t av=std::abs(v);
        if(exp==7&&av==(1<<12)){
            I=3; int32_t ne=exp+2; int32_t nv=gb(raw,4,15)>>2;
            raw=0;
            for(int j=0;j<5;j++){raw=sb(raw,j,j,ne&1);ne>>=1;}
            raw=sb(raw,5,5,v>=0?1:0);
            for(int j=6;j<=15;j++){raw=sb(raw,j,j,nv&1);nv>>=1;}
            incLargeAC(delta,rng);
        } else if(av==(1<<12)){
            exp++; av=1<<11;
            raw=sb(raw,0,2,exp); raw=sb(raw,3,3,v>=0?1:0);
            for(int j=4;j<=15;j++){raw=sb(raw,j,j,av&1);av>>=1;}
        } else{
            raw=sb(raw,3,3,v>=0?1:0); av=std::abs(v);
            for(int j=4;j<=15;j++){raw=sb(raw,j,j,av&1);av>>=1;}}
    }
    void incLargeAC(int32_t delta, uint64_t &rng){
        int32_t exp=gb(raw,0,4), expexp=1<<exp;
        rng=rng*6364136223846793005ULL+1442695040888963407ULL;
        if((int32_t)((uint32_t)(rng>>33)%expexp)!=0) return;
        bool pos=gb(raw,5,5); int32_t v=gb(raw,6,15);
        if(!pos) v=-v; v+=delta; int32_t av=std::abs(v);
        if(av==(1<<10)){
            exp++; av=1<<9; raw=0;
            for(int j=0;j<5;j++){raw=sb(raw,j,j,exp&1);exp>>=1;}
            raw=sb(raw,5,5,v>=0?1:0);
            for(int j=6;j<=15;j++){raw=sb(raw,j,j,av&1);av>>=1;}
        } else{
            raw=sb(raw,5,5,v>=0?1:0);
            for(int j=6;j<=15;j++){raw=sb(raw,j,j,av&1);av>>=1;}}
    }
    void increaseValue(int32_t delta, int idx, uint64_t &rng){
        if(I==0) incByte(delta,idx);
        else if(I==1) incShort(delta);
        else if(I==2) incSmallAC(delta,rng);
        else incLargeAC(delta,rng);}

    // ── Merge helpers (used by SSVSSketch::Merge) ────────────────────────────
    // Collapse the two byte counters into one 16-bit short holding their sum —
    // the same thing incByte() does when one of the bytes overflows.
    void promoteToShort(){
        if(I!=0) return;
        int32_t v=getByteVal(0)+getByteVal(1);          // |v| <= 254, fits easily
        I=1; raw=0; raw=sb(raw,0,0,v>=0?1:0);
        int32_t mv=std::abs(v);
        for(int j=1;j<=15;j++){raw=sb(raw,j,j,mv&1);mv>>=1;}
    }
    // Replay `amount` unit updates into sub-slot idx (unbiased for the AC stages,
    // since each replayed update still goes through the 1/2^exp gate).
    void addValue(int64_t amount, int idx, uint64_t &rng){
        int32_t sg = amount > 0 ? 1 : -1;
        for(int64_t x = 0, n = amount < 0 ? -amount : amount; x < n; x++) increaseValue(sg, idx, rng);
    }
};

// CountMinSketch handles both CM and CU depending on the conservative flag.
// CM just increments all d counters for a flow. CU is more accurate because it
// reads all d first and only increments the ones that are at the minimum,
// so it doesn't overcount as fast. Query is always just the minimum of all d counters.
// counterSize=32, w=8192 at d=4 to fit in 1 Mbit.
struct CountMinSketch {
    int32_t depth,width; bool conservative;
    std::vector<int32_t> table;
    std::vector<uint64_t> seeds;

    CountMinSketch()=default;
    CountMinSketch(int32_t depth_, int32_t width_, bool conservative_,
                   const std::vector<uint64_t> *seedsIn = nullptr)
        : depth(depth_), width(width_), conservative(conservative_),
          table(depth_ * width_, 0) {
        // Copy the provided seeds, or generate fresh random ones if none given
        for (int32_t row = 0; row < depth_; row++)
            seeds.push_back(seedsIn ? (*seedsIn)[row] : RowSeed(row));
    }

    // Compute the column index for flow hash v in row r.
    // XOR with the row's seed makes each row independent, then intHash mixes the bits,
    // then jmod maps the result to [0, width) handling negative values safely.
    int32_t hcol(int32_t v,int32_t r) const{
        return jmod(intHash(v^(int32_t)seeds[r]),width);}

    // Insert a flow. CM: increment all d counters. CU (conservative): first find
    // the minimum counter value across all d rows, then only increment the cells
    // that are already at that minimum. This prevents overestimating hot counters.
    // key.hash() is called once per row, like Java CM.java / CU.java (see FlowKey).
    template<class K> void Insert(const K &key){
        if(!conservative){
            // CM: unconditionally increment one counter per row
            for(int32_t r=0;r<depth;r++) table[r*width+hcol(key.hash(),r)]++;
        } else{
            // CU: read all d counters first, find the minimum value,
            // then only increment the cells that equal the minimum.
            // Java CU.java keeps the d column indexes, so no second hash here.
            int32_t mn=INT32_MAX;
            RowScratch cs(depth);
            for(int32_t r=0;r<depth;r++){cs[r]=hcol(key.hash(),r);mn=std::min(mn,table[r*width+cs[r]]);}
            for(int32_t r=0;r<depth;r++) if(table[r*width+cs[r]]==mn) table[r*width+cs[r]]++;}
    }
    void Insert(int32_t v){ Insert(HashedKey{v}); }


    // CountMin Query
    int64_t Query(int32_t v) const{

        // Max int so a min will always be found
        int32_t mn=INT32_MAX;

        // Check every row in the sketch 
        for(int32_t r=0;r<depth;r++){
            
            // Sketch isn't actually 2d, just offset 1d array
            // hcol() returns the column index for the flow hash v in row r
            mn=std::min(mn,table[r*width+hcol(v,r)]);
        }

        // Return the min or 0 because negatives should never happen 
        return std::max((int64_t)mn,(int64_t)0);
    }




    // Merge another sketch into this one by adding counter-by-counter.
    // Used by DuckDB's parallel aggregate Combine step.
    void Merge(const CountMinSketch &o){
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);
        for(int32_t i=0;i<depth*width;i++) table[i]+=o.table[i];}

    std::string Serialize() const{
        int32_t c=conservative?1:0;
        std::string out(3*4+(int64_t)depth*width*4+depth*8,'\0');
        char*p=&out[0];
        memcpy(p,&depth,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&c,4);p+=4;
        memcpy(p,table.data(),depth*width*4);p+=depth*width*4;
        for(int32_t i=0;i<depth;i++){memcpy(p,&seeds[i],8);p+=8;}
        return out;}
    static CountMinSketch Deserialize(const char*d,size_t){
        int32_t dep,wid,c;
        memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&c,d,4);d+=4;
        CountMinSketch sk(dep,wid,c==1);
        memcpy(sk.table.data(),d,dep*wid*4);d+=dep*wid*4;
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        return sk;}
};
// CountMinState holds one sketch pointer per parallel worker thread.
// DuckDB allocates one State per thread, then Combine() merges them at the end.
// CountMinState: per-thread aggregate state.
// DuckDB allocates one of these per worker thread.
// The sketch pointer starts null and is allocated on the first Operation() call.
struct CountMinState {
    CountMinSketch *sketch = nullptr;
    bool IsSet() const { return sketch != nullptr; }
};
// CountMinBindData is created once at query-bind time and shared across all workers.
// It stores the depth, width, conservative flag, and pre-generated seeds so every
// parallel thread builds sketches with the same hash functions.
// CountMinBindData: created once at query-plan time, shared by all worker threads.
// Holds depth, width, conservative flag, and the pre-generated random seeds.
// All threads use the same seeds so their partial sketches can be merged correctly.
struct CountMinBindData : public duckdb::FunctionData {
    int32_t depth;
    int32_t width;
    bool conservative;
    std::vector<uint64_t> seeds;

    CountMinBindData(int32_t depth_, int32_t width_, bool conservative_)
        : depth(depth_), width(width_), conservative(conservative_) {
        // Generate one random seed per row — these stay fixed for the lifetime of the query
        seeds.resize(depth_);
        for (int32_t i = 0; i < depth_; i++) seeds[i] = RowSeed(i);   // fixed seeds: sketches stay mergeable
    }

    duckdb::unique_ptr<FunctionData> Copy() const override {
        auto copy = duckdb::make_uniq<CountMinBindData>(depth, width, conservative);
        copy->seeds = seeds;
        return copy;
    }

    bool Equals(const FunctionData &other) const override {
        auto &otherBind = other.Cast<CountMinBindData>();
        return depth == otherBind.depth &&
               width == otherBind.width &&
               conservative == otherBind.conservative;
    }
};

// Countmin_agg aggregate lifecylce 
struct CountMinOperation{

    // initialize the state done for each thread, sketch pointer starts null
    template<class S>
    static void Initialize(S &state){
        state.sketch = nullptr;
    }

    // Allocate space for the sketch if not done already by another thread
    template<class S>
    static void InitSketch(S &state, const CountMinBindData &bindData){

        if (!state.IsSet()){

            state.sketch = new CountMinSketch(bindData.depth, bindData.width,

                //conservative update also uses this and needs a flag 
                bindData.conservative, &bindData.seeds);
            }
    }

    // Insert the flow into the sketch. Called once per input row.
    template<class I, class S, class OP>
    static void Operation(S &state, const I &flowString,duckdb::AggregateUnaryInput &unaryInput){

        auto &bindData = unaryInput.input.bind_data->Cast<CountMinBindData>();

        // Initialize the sketch if not done yet
        InitSketch(state, bindData);

        // Insert() defined in CountMinSketch, takes a HashedKey or int32_t
        state.sketch->Insert(FlowKey(flowString.GetData(), flowString.GetSize()));
    }


    // Merge all the sketches each thread made into one sketch 
    template<class S, class OP>
    static void Combine(const S &sourceState, S &targetState, duckdb::AggregateInputData &){

        // Source thread was empty 
        if (!sourceState.IsSet()) return;

        if (!targetState.IsSet()){

            // Target thread also had no rows just take a copy of source
            targetState.sketch = new CountMinSketch(*sourceState.sketch);
            return;
        }

        // Both have partial sketches add source counters into target counter by counter
        // Merge defined in CountMinSketch
        targetState.sketch->Merge(*sourceState.sketch);
    }

    // Turns the finalized sketch into a BLOB 
    template<class T, class S>
    static void Finalize(S &state, T &target, duckdb::AggregateFinalizeData &finalizeData){
        
        if (!state.IsSet()) { finalizeData.ReturnNull(); return; }

        // Serialize turns the sketch into string of bytes (BLOB)
        std::string serializedBytes = state.sketch->Serialize();

        target = duckdb::StringVector::AddStringOrBlob(
            finalizeData.result,
            serializedBytes.data(),
            serializedBytes.size());
    }

    // called after Finalize frees up the memeory 
    template<class S>
    static void Destroy(S &state, duckdb::AggregateInputData &) {
        delete state.sketch;
        state.sketch = nullptr;
    }

    static bool IgnoreNull() { return true; }
};

// CountSketch uses signed counters so noise can cancel out. Each insert
// does +1 or -1 depending on the low bit of the hash. Query returns the median
// across all d rows multiplied by their sign, which gives an unbiased estimate.
// The col comes from hashV>>>1 and the sign comes from hashV&1 so they are
// derived from the same hash call, same as the Java version.
struct CountSketch {
    int32_t depth,width;
    std::vector<int32_t> table;
    std::vector<uint64_t> seeds;

    CountSketch()=default;
    CountSketch(int32_t d,int32_t w,const std::vector<uint64_t>*s=nullptr)
        :depth(d),width(w),table(d*w,0){
        for(int32_t i=0;i<d;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    // Compute the full hash for row r. Both col and sign are derived from this
    // single hash value — col uses the upper bits (>> 1), sign uses the lowest bit.
    // Using one hash for both ensures they're statistically linked as in the Java version.
    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}
    // Column = upper bits of hash after removing the sign bit (unsigned shift right 1)
    int32_t hcol(int32_t v,int32_t r) const {return jcol(hv(v,r),1,width);}
    // Sign = lowest bit of hash (0 → -1, 1 → +1)
    int32_t hsign(int32_t v,int32_t r) const {return (hv(v,r)&1)?1:-1;}

    // Insert: add +1 or -1 to each row's counter depending on the sign hash.
    // The signed nature means noise from different flows partially cancels.
    // One key hash per row, like Java CS.java; column and sign come from it.
    template<class K> void Insert(const K &key){
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(key.hash(),r);
            table[r*width+jcol(h,1,width)]+=(h&1)?1:-1;}}
    void Insert(int32_t v){ Insert(HashedKey{v}); }
    // Query: multiply each counter by the sign hash (to flip sign for negatives),
    // sort the d estimates, and return the median. The median is more robust than
    // the minimum because signed noise is symmetric around zero.
    int64_t Query(int32_t v) const{
        std::vector<int32_t> e(depth);
        for(int32_t r=0;r<depth;r++) e[r]=table[r*width+hcol(v,r)]*hsign(v,r);
        std::sort(e.begin(),e.end());
        int32_t med=(depth%2==1)?e[depth/2]:(e[depth/2-1]+e[depth/2])/2;
        return std::max((int64_t)med,(int64_t)0);}   // clamp negative medians to 0
    void Merge(const CountSketch&o){
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);
        for(int32_t i=0;i<depth*width;i++) table[i]+=o.table[i];}

    std::string Serialize() const{
        std::string out(2*4+(int64_t)depth*width*4+depth*8,'\0');
        char*p=&out[0];
        memcpy(p,&depth,4);p+=4;memcpy(p,&width,4);p+=4;
        memcpy(p,table.data(),depth*width*4);p+=depth*width*4;
        for(int32_t i=0;i<depth;i++){memcpy(p,&seeds[i],8);p+=8;}
        return out;}
    static CountSketch Deserialize(const char*d,size_t){
        int32_t dep,wid; memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;
        CountSketch sk(dep,wid);
        memcpy(sk.table.data(),d,dep*wid*4);d+=dep*wid*4;
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        return sk;}
};
struct CountSketchState{CountSketch*sketch=nullptr;bool IsSet()const{return sketch!=nullptr;}};
struct CountSketchBindData:public duckdb::FunctionData{
    int32_t depth,width;std::vector<uint64_t>seeds;
    CountSketchBindData(int32_t d,int32_t w):depth(d),width(w){
        seeds.resize(d);for(int32_t i=0;i<d;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<CountSketchBindData>(depth,width);
        cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<CountSketchBindData>();return depth==x.depth&&width==x.width;}
};
struct CountSketchOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;}
    template<class S>static void InitSketch(S&s,const CountSketchBindData&b){
        if(!s.IsSet()) s.sketch=new CountSketch(b.depth,b.width,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<CountSketchBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()));}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<CountSketchBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new CountSketch(*src.sketch);return;}
        tgt.sketch->Merge(*src.sketch);}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// CMSCSketch is Count-Min with Self-Adjusting Counters, and CU-SC reuses the same
// struct with conservative=true. SC counters start small and grow as needed
// so the same memory holds more counters initially which reduces collisions.
struct CMSCSketch {
    int32_t depth,width;
    bool conservative=false;
    std::vector<std::vector<SAC64>> table;
    std::vector<uint64_t> seeds;

    CMSCSketch()=default;
    CMSCSketch(int32_t d,int32_t w,bool c=false,const std::vector<uint64_t>*s=nullptr)
        :depth(d),width(w),conservative(c),table(d,std::vector<SAC64>(w)){
        for(int32_t i=0;i<d;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}

    // Same steps and key hashes as Java CM_SC.java / CU_SC.java (see FlowKey).
    template<class K> void Insert(const K &key){
        if(!conservative){
            for(int32_t r=0;r<depth;r++){
                int32_t h=hv(key.hash(),r),c=jcol(h,2,width),su=h&3;
                table[r][c].increaseValue(1,su);}
        } else{
            // CU_SC conservative: read every row's counter and find the minimum...
            int32_t mn=INT32_MAX;
            RowScratch value(depth);
            for(int32_t r=0;r<depth;r++){
                int32_t h=hv(key.hash(),r);
                value[r]=table[r][jcol(h,2,width)].getValue(h&3);
                mn=std::min(mn,value[r]);}
            // ...then hash each row at the minimum again and increase it (as Java does).
            for(int32_t r=0;r<depth;r++)
                if(value[r]==mn){
                    int32_t h=hv(key.hash(),r);
                    table[r][jcol(h,2,width)].increaseValue(1,h&3);}}
    }
    void Insert(int32_t v){ Insert(HashedKey{v}); }
    int64_t Query(int32_t v) const{
        int32_t mn=INT32_MAX;
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(v,r);
            mn=std::min(mn,table[r][jcol(h,2,width)].getValue(h&3));}
        return std::max((int64_t)mn,(int64_t)0);}

    void Merge(CMSCSketch&o){
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);
        for(int32_t r=0;r<depth;r++)
            for(int32_t j=0;j<width;j++)
                table[r][j].mergeFrom(o.table[r][j]);
    }

    std::string Serialize() const{
        int32_t c=conservative?1:0;
        size_t sz=3*4+(size_t)depth*8+(size_t)depth*width*SAC64::sz();
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&depth,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&c,4);p+=4;
        for(int32_t i=0;i<depth;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t r=0;r<depth;r++) for(int32_t j=0;j<width;j++) table[r][j].ser(p);
        return out;}
    static CMSCSketch Deserialize(const char*d,size_t){
        int32_t dep,wid,c;
        memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&c,d,4);d+=4;
        CMSCSketch sk(dep,wid,c==1);
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t r=0;r<dep;r++) for(int32_t j=0;j<wid;j++) sk.table[r][j]=SAC64::deser(d);
        return sk;}
};
struct CMSCState{CMSCSketch*sketch=nullptr;bool IsSet()const{return sketch!=nullptr;}};
struct CMSCBindData:public duckdb::FunctionData{
    int32_t depth,width;bool conservative;std::vector<uint64_t>seeds;
    CMSCBindData(int32_t d,int32_t w,bool c=false):depth(d),width(w),conservative(c){
        seeds.resize(d);for(int32_t i=0;i<d;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<CMSCBindData>(depth,width,conservative);
        cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        // FIX: must compare `conservative` too. CM-SC and CU-SC share the same
        // aggregate callbacks, so if this returned true DuckDB would treat
        // cmsc_agg(...) and cusc_agg(...) in one query as duplicates and hand
        // back the CM-SC sketch for both.
        auto&x=o.Cast<CMSCBindData>();return depth==x.depth&&width==x.width&&conservative==x.conservative;}
};
struct CMSCOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;}
    template<class S>static void InitSketch(S&s,const CMSCBindData&b){
        if(!s.IsSet()) s.sketch=new CMSCSketch(b.depth,b.width,b.conservative,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<CMSCBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()));}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<CMSCBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new CMSCSketch(*src.sketch);return;}
        tgt.sketch->Merge(*const_cast<CMSCSketch*>(src.sketch));}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// CSSCSketch is Count Sketch with Self-Adjusting Counters.
// Same idea as CS but with SC counters instead of plain 32-bit ints.
// ─── CSSCSketch  (Count Sketch with Self-Adjusting Counters) ─────────────────
// CS-SC uses signed SAC64 counters in a d×w 2D table.
// Each insert writes +1 or -1 into a sub-slot of one SAC64 cell per row.
// Query uses the median of signed estimates, same as plain CountSketch.
// counterSize=36, w=(M/36)/d=7281 at d=4.
struct CSSCSketch {
    int32_t depth,width;
    std::vector<std::vector<SAC64>> table;  // d rows × w SAC64 cells
    std::vector<uint64_t> seeds;            // one seed per row for hash independence

    CSSCSketch()=default;
    CSSCSketch(int32_t d,int32_t w,const std::vector<uint64_t>*s=nullptr)
        :depth(d),width(w),table(d,std::vector<SAC64>(w)){
        for(int32_t i=0;i<d;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}

    // One key hash per row, like Java CS_SC.java (see FlowKey).
    template<class K> void Insert(const K &key){
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(key.hash(),r);
            int32_t c=jcol(h,3,width),sg=(h&4)?1:-1,su=h&3;
            table[r][c].increaseValue(sg,su);}
    }
    void Insert(int32_t v){ Insert(HashedKey{v}); }
    int64_t Query(int32_t v) const{
        std::vector<int32_t> e(depth);
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(v,r);
            e[r]=((h&4)?1:-1)*table[r][jcol(h,3,width)].getValue(h&3);}
        std::sort(e.begin(),e.end());
        int32_t med=(depth%2==1)?e[depth/2]:(e[depth/2-1]+e[depth/2])/2;
        return std::max((int64_t)med,(int64_t)0);}

    void Merge(CSSCSketch&o){
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);
        for(int32_t r=0;r<depth;r++)
            for(int32_t j=0;j<width;j++)
                table[r][j].mergeFrom(o.table[r][j]);
    }

    std::string Serialize() const{
        int32_t c=0;
        size_t sz=3*4+(size_t)depth*8+(size_t)depth*width*SAC64::sz();
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&depth,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&c,4);p+=4;
        for(int32_t i=0;i<depth;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t r=0;r<depth;r++) for(int32_t j=0;j<width;j++) table[r][j].ser(p);
        return out;}
    static CSSCSketch Deserialize(const char*d,size_t){
        int32_t dep,wid,c;
        memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&c,d,4);d+=4;
        CSSCSketch sk(dep,wid);
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t r=0;r<dep;r++) for(int32_t j=0;j<wid;j++) sk.table[r][j]=SAC64::deser(d);
        return sk;}
};
struct CSSCState{CSSCSketch*sketch=nullptr;bool IsSet()const{return sketch!=nullptr;}};
struct CSSCBindData:public duckdb::FunctionData{
    int32_t depth,width;std::vector<uint64_t>seeds;
    CSSCBindData(int32_t d,int32_t w):depth(d),width(w){
        seeds.resize(d);for(int32_t i=0;i<d;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<CSSCBindData>(depth,width);
        cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<CSSCBindData>();return depth==x.depth&&width==x.width;}
};
struct CSSCOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;}
    template<class S>static void InitSketch(S&s,const CSSCBindData&b){
        if(!s.IsSet()) s.sketch=new CSSCSketch(b.depth,b.width,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<CSSCBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()));}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<CSSCBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new CSSCSketch(*src.sketch);return;}
        tgt.sketch->Merge(*const_cast<CSSCSketch*>(src.sketch));}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// CMACSketch is Count-Min with Active Counters, and CU-AC reuses it with conservative=true.
// AC16 counters are only 16 bits so we get twice as many counters for the same memory.
// ─── CMACSketch  (Count-Min / Counter Update with Active Counters) ────────────
// CM-AC / CU-AC use probabilistic AC16 counters in a d×w 2D table.
// Packing two counters per 32 bits doubles the number of counters for the same
// memory, reducing collisions. The trade-off is additional estimation variance.
// counterSize=16, w=(M/16)/d=16384 at d=4.

// CountMin with Active Counters is reused for Constervative Update Active Counters by setting conservative=true.
struct CMACSketch{

    // Table Demensions 
    int32_t depth,width;

    // false=CM-AC, true=CU-AC
    bool conservative=false;

    // Table is 2D Vector of AC16 counters, one hash per row, one column per counter
    std::vector<std::vector<AC16>> table;

    // Hash Seeds for each row so they are different
    std::vector<uint64_t> seeds;

    // Shared probability for Active Counter updates
    mutable uint64_t rng;                  

    // Not Counter Update or Conservative Update 
    CMACSketch()=default;
    CMACSketch(int32_t d,int32_t w,bool c=false,const std::vector<uint64_t>*s=nullptr)
        :depth(d),width(w),conservative(c),table(d,std::vector<AC16>(w)),rng(RandomSeed()){
        for(int32_t i=0;i<d;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}


    template<class K> 
    void Insert(const K &key){

        // If not conservative update
        if(!conservative){
            for(int32_t r=0;r<depth;r++){

                // Use hash function with jcol() to get a column index for the counter in this row
                // increase the counter there using AC 16's increaseValue() rng is the probabilty 
                table[r][jcol(hv(key.hash(),r),2,width)].increaseValue(1,rng);
            }
        } 


        else{
            // CU_AC conservative: read every row's counter and find the minimum...
            int32_t mn=INT32_MAX;
            RowScratch value(depth);
            for(int32_t r=0;r<depth;r++){
                value[r]=table[r][jcol(hv(key.hash(),r),2,width)].getValue(0);
                mn=std::min(mn,value[r]);}
            // ...then hash each row at the minimum again and increase it (as Java does).
            for(int32_t r=0;r<depth;r++)
                if(value[r]==mn)
                    table[r][jcol(hv(key.hash(),r),2,width)].increaseValue(1,rng);}
    }
    void Insert(int32_t v){ Insert(HashedKey{v}); }


    // In CMACSketch Struct 
    // Return the minimum counter value across all rows 
    int64_t Query(int32_t v) const{

        // Max val to ensure we get a min
        int32_t mn=INT32_MAX;

        // For each row if the counter is new min then set it
        for(int32_t r=0;r<depth;r++){
            mn = std::min(mn,table[r][jcol(hv(v,r),2,width)].getValue(0));
        }

        // Return the min
        return std::max((int64_t)mn,(int64_t)0);}



    void Merge(CMACSketch&o){

        // Check if tables are compatible for merging, same depth, width, and seeds
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);

        // For ever row and column 
        for(int32_t r=0;r<depth;r++){
            for(int32_t j=0;j<width;j++){

                int32_t ov=o.table[r][j].getValue(0);

                // If the other sketch has a positive value then increase the current sketch's counter by that amount
                if(ov>0){
                    for(int32_t x=0;x<ov;x++){ 
                        table[r][j].increaseValue(1,rng);
                    }
                }
                // If the other sketch has a negative value then decrease the current sketch's counter by that amount
                else if(ov<0) {
                    for(int32_t x=0;x<-ov;x++){ 
                        table[r][j].increaseValue(-1,rng);
                    }
                }
            }
        }
    }

    std::string Serialize() const{

        // If conservative update then set c=1 else c=0
        int32_t c = conservative? 1:0;

        // Calculate the size of the serialized string
        // 3*4 for depth, width, c + depth*8 for seeds + depth*width*AC16::sz() for table
        size_t sz=3*4+(size_t)depth*8+(size_t)depth*width*AC16::sz();

        // Create a string of the calculated size and fill it with null characters
        std::string out(sz,'\0'); char*p=&out[0];

        // Copy depth, width, and c into the string
        memcpy(p,&depth,4);
        p+=4;
        memcpy(p,&width,4);
        p+=4;
        memcpy(p,&c,4);
        p+=4;
        
        for(int32_t i=0;i<depth;i++){
            memcpy(p,&seeds[i],8);
            p+=8;
        }

        for(int32_t r=0;r<depth;r++){
            for(int32_t j=0;j<width;j++){
                table[r][j].ser(p);
            }
        }

        return out;}



    static CMACSketch Deserialize(const char*d,size_t){
        int32_t dep,wid,c;
        memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&c,d,4);d+=4;
        CMACSketch sk(dep,wid,c==1);
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t r=0;r<dep;r++) for(int32_t j=0;j<wid;j++) sk.table[r][j]=AC16::deser(d);
        return sk;}
};
struct CMACState{CMACSketch*sketch=nullptr;bool IsSet()const{return sketch!=nullptr;}};
struct CMACBindData:public duckdb::FunctionData{
    int32_t depth,width;bool conservative;std::vector<uint64_t>seeds;
    CMACBindData(int32_t d,int32_t w,bool c=false):depth(d),width(w),conservative(c){
        seeds.resize(d);for(int32_t i=0;i<d;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<CMACBindData>(depth,width,conservative);
        cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        // FIX: must compare `conservative` too (see CMSCBindData::Equals) —
        // otherwise cuac_agg(...) is deduplicated into cmac_agg(...) in one query.
        auto&x=o.Cast<CMACBindData>();return depth==x.depth&&width==x.width&&conservative==x.conservative;}
};

// Creating a new sketch for CMAC done for each thread 
struct CMACOperation{

    // Initialize the state done for each thread 
    template<class S>
    static void Initialize(S&s){
        s.sketch=nullptr;
    }

    // Allocate space for the sketch if not done by antoher thread 
    template<class S>
    static void InitSketch(S&s,const CMACBindData&b){
        if(!s.IsSet()){
             s.sketch=new CMACSketch(b.depth,b.width,b.conservative,&b.seeds);
        }
    }

    // Called for each row in the input, inserts the row into the sketch
    template<class I,class S,class OP>
    static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){

        // Bind data is the sketch parameters (depth, width, seeds) and is shared across threads
        auto&b=u.input.bind_data->Cast<CMACBindData>();

        // Calling the init sketch function nothing will happen if already done
        InitSketch(s,b);

        // Insert defined in CMACSketch, which does the hashing and inserts into the table
        // Called for each row in the input
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()));
    }

    // Merge two sketches together, called when combining threads
    template<class S,class OP>
    static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){

        // If empty do nothing
        if(!src.IsSet())return;

        // Empty target sketch, copy the source sketch
        if(!tgt.IsSet()){
            tgt.sketch=new CMACSketch(*src.sketch);return;
        }

        // Merge the two sketches together, defined in CMACSketch
        tgt.sketch->Merge(*const_cast<CMACSketch*>(src.sketch));
    
    }

    // Writing the sketch to result vector as a BLOB
    template<class T,class S>
    static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){

        // If empty return null
        if(!s.IsSet()){
            fd.ReturnNull();
            return;
        }

        // Serialize defined in CMACSketch, returns a string with the serialized sketch
        auto b=s.sketch->Serialize();

        // Sets the target vector to the serialized sketch, as a BLOB
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());
    }


    template<class S>
    static void Destroy(S&s,duckdb::AggregateInputData&){
        // Free up the sketch memory, called when the aggregate is done
        delete s.sketch;s.sketch=nullptr;
    }



    static bool IgnoreNull(){return true;}








        template<class I,class S,class OP>
    static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<CMACBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key);}
};

// CSACSketch is Count Sketch with Active Counters.
// Signed probabilistic increments with the same +/-1 trick as plain CS.
// ─── CSACSketch  (Count Sketch with Active Counters) ─────────────────────────
// CS-AC uses signed AC16 counters. Each insert does a probabilistic +1 or -1.
// Query returns the median signed estimate, combining AC16's memory efficiency
// with CountSketch's noise cancellation.
// counterSize=16, w=(M/16)/d=16384 at d=4.
struct CSACSketch {
    int32_t depth,width;
    std::vector<std::vector<AC16>> table;  // d rows × w AC16 counters
    std::vector<uint64_t> seeds;
    mutable uint64_t rng;                  // LCG state for AC16 probabilistic gate

    CSACSketch()=default;
    CSACSketch(int32_t d,int32_t w,const std::vector<uint64_t>*s=nullptr)
        :depth(d),width(w),table(d,std::vector<AC16>(w)),rng(RandomSeed()){
        for(int32_t i=0;i<d;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}

    // One key hash per row, like Java CS_AC.java (see FlowKey).
    template<class K> void Insert(const K &key){
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(key.hash(),r),c=jcol(h,2,width),sg=(h&1)?1:-1;
            table[r][c].increaseValue(sg,rng);}
    }
    void Insert(int32_t v){ Insert(HashedKey{v}); }
    int64_t Query(int32_t v) const{
        std::vector<int32_t> e(depth);
        for(int32_t r=0;r<depth;r++){
            int32_t h=hv(v,r);
            e[r]=((h&1)?1:-1)*table[r][jcol(h,2,width)].getValue(0);}
        std::sort(e.begin(),e.end());
        int32_t med=(depth%2==1)?e[depth/2]:(e[depth/2-1]+e[depth/2])/2;
        return std::max((int64_t)med,(int64_t)0);}

    void Merge(CSACSketch&o){
        CheckMergeable(depth,width,seeds,o.depth,o.width,o.seeds);
        for(int32_t r=0;r<depth;r++)
            for(int32_t j=0;j<width;j++){
                int32_t ov=o.table[r][j].getValue(0);
                if(ov>0) for(int32_t x=0;x<ov;x++) table[r][j].increaseValue(1,rng);
                else if(ov<0) for(int32_t x=0;x<-ov;x++) table[r][j].increaseValue(-1,rng);}
    }

    std::string Serialize() const{
        int32_t c=0;
        size_t sz=3*4+(size_t)depth*8+(size_t)depth*width*AC16::sz();
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&depth,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&c,4);p+=4;
        for(int32_t i=0;i<depth;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t r=0;r<depth;r++) for(int32_t j=0;j<width;j++) table[r][j].ser(p);
        return out;}
    static CSACSketch Deserialize(const char*d,size_t){
        int32_t dep,wid,c;
        memcpy(&dep,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&c,d,4);d+=4;
        CSACSketch sk(dep,wid);
        for(int32_t i=0;i<dep;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t r=0;r<dep;r++) for(int32_t j=0;j<wid;j++) sk.table[r][j]=AC16::deser(d);
        return sk;}
};
struct CSACState{CSACSketch*sketch=nullptr;bool IsSet()const{return sketch!=nullptr;}};
struct CSACBindData:public duckdb::FunctionData{
    int32_t depth,width;std::vector<uint64_t>seeds;
    CSACBindData(int32_t d,int32_t w):depth(d),width(w){
        seeds.resize(d);for(int32_t i=0;i<d;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<CSACBindData>(depth,width);
        cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<CSACBindData>();return depth==x.depth&&width==x.width;}
};
struct CSACOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;}
    template<class S>static void InitSketch(S&s,const CSACBindData&b){
        if(!s.IsSet()) s.sketch=new CSACSketch(b.depth,b.width,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<CSACBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()));}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<CSACBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new CSACSketch(*src.sketch);return;}
        tgt.sketch->Merge(*const_cast<CSACSketch*>(src.sketch));}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// RCSSketch is Randomized Counter Sharing. The key difference from CM is that
// for each packet we only write to ONE randomly chosen row instead of all d.
// That means only one memory write per packet regardless of how many rows we have.
// Query sums all d counters for the flow and subtracts n/w as a noise estimate,
// where n is the total number of packets inserted.
// counterSize=32, d=4 rows, w=8192 columns per row to fit in 1 Mbit (same as Java RCS.java).
// ─── RCSSketch  (Randomized Counter Sharing, 2D d×w array) ───────────────────
// Single-update sketch. One counter written per packet (vs d for CM).
// 2D table: l=d rows × width=w columns. Each insert picks a random row.
// counterSize=32, l=4 rows, w=8192 columns (fills 1 Mbit, same as Java RCS.java).
struct RCSSketch {
    int32_t l,width;                            // l=depth(rows), width=columns per row
    int64_t n=0;                                // total packets inserted (for noise estimate)
    std::vector<std::vector<int32_t>> table;    // l rows × width int32 counters
    std::vector<uint64_t> seeds;                // one seed per row

    RCSSketch()=default;
    RCSSketch(int32_t ll,int32_t w,const std::vector<uint64_t>*s=nullptr)
        :l(ll),width(w),n(0),table(ll,std::vector<int32_t>(w,0)){
        for(int32_t i=0;i<ll;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hcol(int32_t v,int32_t r) const{
        return jcol(intHash(v^(int32_t)seeds[r]),1,width);}

    // Insert: pick a random row using LCG, increment that row's column counter.
    // Only one counter is written per packet (single-update property of RCS).
    // Single-update sketch: one key hash per packet, as in Java (see FlowKey).
    template<class K> void Insert(const K &key,uint64_t&rs){ Insert(key.hash(),rs); }
    void Insert(int32_t v,uint64_t&rs){
        int32_t r=LcgPick(rs,l);               // random row r in [0, l) — FIX: high bits (was rs % l)
        table[r][hcol(v,r)]++; n++;}           // increment cell, count total packets
    // Query: sum all l row counters then subtract noise = n/width.
    // The n/width term is the expected background collision noise per column.
    int64_t Query(int32_t v) const{
        int64_t sum=0;
        for(int32_t i=0;i<l;i++) sum+=table[i][hcol(v,i)];   // accumulate l counters
        int64_t noise=(width>0)?n/width:0;     // expected noise (total pkts / width)
        return std::max(sum-noise,(int64_t)0);}  // clamp negative to 0
    void Merge(const RCSSketch&o){
        CheckMergeable(l,width,seeds,o.l,o.width,o.seeds);
        for(int32_t i=0;i<l;i++)
            for(int32_t j=0;j<width;j++) table[i][j]+=o.table[i][j];
        n+=o.n;}

    std::string Serialize() const{
        size_t sz=2*4+8+(size_t)l*8+(size_t)l*width*4;
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&l,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&n,8);p+=8;
        for(int32_t i=0;i<l;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t i=0;i<l;i++)
            for(int32_t j=0;j<width;j++){memcpy(p,&table[i][j],4);p+=4;}
        return out;}
    static RCSSketch Deserialize(const char*d,size_t){
        int32_t ll,wid; int64_t nv;
        memcpy(&ll,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&nv,d,8);d+=8;
        RCSSketch sk(ll,wid); sk.n=nv;
        for(int32_t i=0;i<ll;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t i=0;i<ll;i++)
            for(int32_t j=0;j<wid;j++){memcpy(&sk.table[i][j],d,4);d+=4;}
        return sk;}
};
struct RCSState{RCSSketch*sketch=nullptr;uint64_t rand_state=0;
    bool IsSet()const{return sketch!=nullptr;}};
struct RCSBindData:public duckdb::FunctionData{
    int32_t l,width;std::vector<uint64_t>seeds;
    RCSBindData(int32_t lv,int32_t w):l(lv),width(w){
        seeds.resize(lv);for(int32_t i=0;i<lv;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<RCSBindData>(l,width);cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<RCSBindData>();return l==x.l&&width==x.width;}
};
struct RCSOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;s.rand_state=RandomSeed();}
    template<class S>static void InitSketch(S&s,const RCSBindData&b){
        if(!s.IsSet()) s.sketch=new RCSSketch(b.l,b.width,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<RCSBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()),s.rand_state);}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<RCSBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key,s.rand_state);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new RCSSketch(*src.sketch);return;}
        tgt.sketch->Merge(*src.sketch);}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// ─── RCSACSketch  (Randomized Counter Sharing with Active Counters) ───────────
// Matches Java RCS_AC.java. counterSize=8, d=512 seeds, w=M/8=131,072.
//
// RCS-AC replaces the plain int32 counters with 8-bit AC8 counters, allowing
// a much larger array (131K slots vs 655 for RCS). The trade-off is that AC8
// introduces probabilistic counting error on top of the collision noise.
//
// Unlike RCS which has a true 2D array, RCS-AC is a FLAT 1D array of w AC8 counters
// shared across all d=512 seeds. Each insert picks one of d seeds and hashes to a
// column using col = hashV % w (no bit shift — RCS_AC.java uses the full hash mod w).
//
// Query formula: (w*d)/(w-d) * (sum/d - n/w)
// This is derived from the expected value equation for a single random counter shared
// across d seeds. The (w*d)/(w-d) scale factor corrects for the fact that the same
// column may be selected by multiple seeds for the same flow.
// ─── RCSACSketch  (Randomized Counter Sharing with Active Counters) ───────────
// Flat 1D array of AC8 counters. l=512 seeds, width=M/8=131,072 counters.
// Unlike RCS's 2D table, all l seeds map into the same flat array.
// Uses the full hash mod w (no bit shift) per Java RCS_AC.java.
struct RCSACSketch {
    int32_t l,width;           // l=number of seeds, width=flat array size
    int64_t n=0;               // total packets for noise formula
    std::vector<AC8> arr;      // flat 1D array of width AC8 counters
    std::vector<uint64_t> seeds;  // l seeds for l independent hash functions
    mutable uint64_t rng;      // LCG state for AC8 probabilistic updates

    RCSACSketch()=default;
    RCSACSketch(int32_t ll,int32_t w,const std::vector<uint64_t>*s=nullptr)
        :l(ll),width(w),n(0),arr(w),rng(RandomSeed()){
        for(int32_t i=0;i<ll;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hcol(int32_t v,int32_t i) const{
        return jmod(intHash(v^(int32_t)seeds[i]),width);}

    // Insert: pick a random seed index, increment the corresponding AC8 counter.
    // Uses full hash mod w (no bit shift) — RCS_AC.java uses plain modulo.
    // Single-update sketch: one key hash per packet, as in Java (see FlowKey).
    template<class K> void Insert(const K &key,uint64_t&rs){ Insert(key.hash(),rs); }
    void Insert(int32_t v,uint64_t&rs){
        int32_t i=LcgPick(rs,l);               // random seed index in [0, l) — FIX: high bits (was rs % l)
        arr[hcol(v,i)].increaseValue(1,rng); n++;}  // probabilistic AC8 increment
    // Query using Java's formula: (w*l)/(w-l) * (sum/l - n/w)
    // Corrects for bias introduced by the single-counter-per-seed sharing scheme.
    int64_t Query(int32_t v) const{
        int64_t sum=0;
        for(int32_t i=0;i<l;i++) sum+=arr[hcol(v,i)].getValue(0);  // sum l counters
        if(l>=width) return std::max(sum,(int64_t)0);  // guard against division by zero
        double est=(double)width*l/(width-l)*((double)sum/l-(double)n/width);  // correction formula
        return std::max((int64_t)est,(int64_t)0);}
    void Merge(RCSACSketch&o){
        CheckMergeable(l,width,seeds,o.l,o.width,o.seeds);
        for(int32_t i=0;i<width;i++){
            int32_t ov=o.arr[i].getValue(0);
            if(ov>0) for(int32_t x=0;x<ov;x++) arr[i].increaseValue(1,rng);}
        n+=o.n;}

    std::string Serialize() const{
        size_t sz=2*4+8+(size_t)l*8+(size_t)width*AC8::sz();
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&l,4);p+=4;memcpy(p,&width,4);p+=4;memcpy(p,&n,8);p+=8;
        for(int32_t i=0;i<l;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t i=0;i<width;i++) arr[i].ser(p);
        return out;}
    static RCSACSketch Deserialize(const char*d,size_t){
        int32_t ll,wid; int64_t nv;
        memcpy(&ll,d,4);d+=4;memcpy(&wid,d,4);d+=4;memcpy(&nv,d,8);d+=8;
        RCSACSketch sk(ll,wid); sk.n=nv;
        for(int32_t i=0;i<ll;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t i=0;i<wid;i++) sk.arr[i]=AC8::deser(d);
        return sk;}
};
struct RCSACState{RCSACSketch*sketch=nullptr;uint64_t rand_state=0;
    bool IsSet()const{return sketch!=nullptr;}};
struct RCSACBindData:public duckdb::FunctionData{
    int32_t l,width;std::vector<uint64_t>seeds;
    RCSACBindData(int32_t lv,int32_t w):l(lv),width(w){
        seeds.resize(lv);for(int32_t i=0;i<lv;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<RCSACBindData>(l,width);cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<RCSACBindData>();return l==x.l&&width==x.width;}
};
struct RCSACOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;s.rand_state=RandomSeed();}
    template<class S>static void InitSketch(S&s,const RCSACBindData&b){
        if(!s.IsSet()) s.sketch=new RCSACSketch(b.l,b.width,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<RCSACBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()),s.rand_state);}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<RCSACBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key,s.rand_state);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new RCSACSketch(*src.sketch);return;}
        tgt.sketch->Merge(*const_cast<RCSACSketch*>(src.sketch));}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// SSVSSketch is the main contribution of the paper. It uses variable counters
// that start as 8-bit byte pairs and grow as needed, and only does a single
// memory write per packet like RCS. The query uses a min-gap algorithm:
// sort the d estimates, find the two closest ones, and only average in other
// estimates that fall within noise/k of that pair. This filters out outliers
// caused by heavy hitters sharing a counter slot.
// counterSize=18, m=58254 counters, d=4 seeds, k=4.
// ─── SSVSSketch  (Single-Update Sketch with Variable Counter Structure) ────────
// The paper's main contribution. Uses VC (VariableCounter) cells that start
// as byte pairs and promote to larger types as needed. Single-update (one cell
// written per packet). Query uses min-gap algorithm for better accuracy.
// counterSize=18 bits, m=M/18=58,254 cells, l=4 seeds.
struct SSVSSketch {
    int32_t l;       // number of hash rows (d=4 by default)
    int32_t m;       // number of VC cells in the flat array (58,254 by default)
    int32_t k;       // noise divisor for query interval (k=4 by default)
    int32_t noise=0; // cached noise level (computed lazily on first Query call)
    // FIX: whether `noise` is known. 0 is a legitimate noise level for a nearly
    // empty sketch, so `noise==0` alone can't mean "not computed"; before this,
    // such sketches recomputed the noise (20,000 samples) on EVERY query.
    bool noiseReady=false;
    std::vector<VC> C;            // flat array of m VariableCounter cells
    std::vector<uint64_t> seeds;  // l seeds for l independent hash rows

    SSVSSketch()=default;
    SSVSSketch(int32_t ll,int32_t mm,int32_t kk,const std::vector<uint64_t>*s=nullptr)
        :l(ll),m(mm),k(kk),noise(0),C(mm){
        for(int32_t i=0;i<ll;i++) seeds.push_back(s?(*s)[i]:RowSeed(i));}

    int32_t hv(int32_t v,int32_t r) const {return intHash(v^(int32_t)seeds[r]);}

    // Single-update sketch: one key hash per packet, as in Java (see FlowKey).
    template<class K> void Insert(const K &key,uint64_t&rs){ Insert(key.hash(),rs); }
    void Insert(int32_t v,uint64_t&rng){
        int32_t r=LcgPick(rng,l);             // FIX: high bits (was rng % l)
        int32_t h=hv(v,r);
        int32_t delta=(h&1)?1:-1;
        int32_t h1=(h>>1)&1;
        int32_t col=((int32_t)((uint32_t)h>>2)%m+m)%m;
        C[col].increaseValue(delta,h1,rng);}

    int32_t readSigned(int32_t v,int32_t i) const{
        int32_t h=hv(v,i);
        int32_t delta=(h&1)?1:-1, h1=(h>>1)&1;
        int32_t col=((int32_t)((uint32_t)h>>2)%m+m)%m;
        return delta*C[col].getValue(h1);}

    // computeNoise estimates the background noise level by querying 20000 fake flows
    // that were never inserted. Their average estimate is the noise floor.
    void computeNoise(){
        // 20000 fake flows, same as the Java version
        int64_t sum=0; uint64_t rng=12345678ULL;
        for(int32_t i=0;i<20000;i++){
            rng=rng*6364136223846793005ULL+1442695040888963407ULL;
            int32_t j=(int32_t)(rng%(uint64_t)m);
            rng=rng*6364136223846793005ULL+1442695040888963407ULL;
            int32_t d2=((int32_t)rng%2==0)?-1:1;
            rng=rng*6364136223846793005ULL+1442695040888963407ULL;
            int32_t h1=(int32_t)(rng%2);
            int32_t tv=d2*C[j].getValue(h1);
            if(tv<0) tv=0; sum+=tv;}
        noise=(int32_t)(sum/20000); noiseReady=true;}

    // SSVS-2 query algorithm. Matches Java SSVS_2.estimateSize() exactly.
    // Unlike CM which just takes the minimum, SSVS-2 uses a min-gap approach
    // to find the most consistent pair of estimates and filters outliers by noise.
    int64_t Query(int32_t v){
        if(!noiseReady){                // first query on this object:
            if(noise==0) computeNoise();  // not stored in the sketch, so compute it once
            noiseReady=true; }
        // Collect d signed estimates, one per row
        std::vector<int32_t> vals(l);
        for(int32_t i=0;i<l;i++) vals[i]=readSigned(v,i);
        // Degenerate cases for very small d
        if(l==1) return std::max((int64_t)vals[0],(int64_t)0);
        if(l==2) return std::max((int64_t)(vals[0]+vals[1]),(int64_t)0);
        // Sort the d estimates to find the min-gap pair
        std::vector<int32_t> sv=vals; std::sort(sv.begin(),sv.end());
        // Find the pair of adjacent sorted values with the smallest gap.
        // The smallest gap identifies the two estimates most likely to be accurate
        // (outliers caused by hash collisions will be far from the true value).
        int32_t minGap=INT32_MAX,minIdx=0;
        for(int32_t i=0;i<l-1;i++){
            int32_t gap=sv[i+1]-sv[i];
            if(gap<minGap){minGap=gap;minIdx=i;}}
        // Start the estimate as the sum of the min-gap pair
        int64_t est=(int64_t)sv[minIdx]+sv[minIdx+1];
        int32_t useful=2;            // count how many estimates we use
        int32_t hn=noise/k;          // half-noise threshold = noise / k (k=4 by default)
        // Include lower estimates that fall within ±noise/k of the lower anchor
        for(int32_t i=0;i<minIdx;i++)
            if(sv[i]>sv[minIdx]-hn&&sv[i]<sv[minIdx]+hn){est+=sv[i];useful++;}
        // Include upper estimates that fall within ±noise/k of the upper anchor
        for(int32_t i=l-1;i>minIdx+1;i--)
            if(sv[i]>sv[minIdx+1]-hn&&sv[i]<sv[minIdx+1]+hn){est+=sv[i];useful++;}
        // Scale the sum by d/useful to account for using fewer than d estimates
        return std::max(est*l/useful,(int64_t)0);}

    // FIX: the old merge did  ov = getValue(0) + getValue(1)  and added ov to sub-slot 0.
    //  * Once a VC has promoted (I>=1: short / small AC / large AC) the two
    //    sub-slots are ONE shared counter, so getValue(0) == getValue(1) and the
    //    value was added TWICE. Heavy flows (the only ones that promote) came out
    //    ~+25% too high whenever DuckDB built the sketch on more than one thread.
    //  * While I==0 the two bytes are independent signed counters; summing them
    //    and dumping the sum into sub-slot 0 moved sub-slot 1's packets into
    //    sub-slot 0, corrupting both.
    // Now: unpromoted source → merge each byte into its own sub-slot; promoted
    // source → promote the target the same way, then add the single value once.
    void Merge(const SSVSSketch&o){
        CheckMergeable(l,m,seeds,o.l,o.m,o.seeds);
        uint64_t rs=RandomSeed();
        for(int32_t j=0;j<m;j++){
            const VC &src=o.C[j];
            if(src.I==0){
                C[j].addValue(src.getByteVal(0),0,rs);
                C[j].addValue(src.getByteVal(1),1,rs);
            } else{
                C[j].promoteToShort();
                C[j].addValue(src.getValue(0),0,rs);
            }
        }
        computeNoise();   // noise floor of the merged sketch (stored when serialized)
    }

    std::string Serialize() const{
        size_t sz=4*4+(size_t)l*8+(size_t)m*2*4;
        std::string out(sz,'\0'); char*p=&out[0];
        memcpy(p,&l,4);p+=4;memcpy(p,&m,4);p+=4;
        memcpy(p,&k,4);p+=4;memcpy(p,&noise,4);p+=4;
        for(int32_t i=0;i<l;i++){memcpy(p,&seeds[i],8);p+=8;}
        for(int32_t j=0;j<m;j++){memcpy(p,&C[j].raw,4);p+=4;memcpy(p,&C[j].I,4);p+=4;}
        return out;}
    static SSVSSketch Deserialize(const char*d,size_t){
        int32_t lv,mv,kv,nv;
        memcpy(&lv,d,4);d+=4;memcpy(&mv,d,4);d+=4;
        memcpy(&kv,d,4);d+=4;memcpy(&nv,d,4);d+=4;
        SSVSSketch sk(lv,mv,kv); sk.noise=nv;
        for(int32_t i=0;i<lv;i++){memcpy(&sk.seeds[i],d,8);d+=8;}
        for(int32_t j=0;j<mv;j++){memcpy(&sk.C[j].raw,d,4);d+=4;memcpy(&sk.C[j].I,d,4);d+=4;}
        return sk;}
};
struct SSVSState{SSVSSketch*sketch=nullptr;uint64_t rand_state=0;
    bool IsSet()const{return sketch!=nullptr;}};
struct SSVSBindData:public duckdb::FunctionData{
    int32_t l,m,k;std::vector<uint64_t>seeds;
    SSVSBindData(int32_t lv,int32_t mv,int32_t kv):l(lv),m(mv),k(kv){
        seeds.resize(lv);for(int32_t i=0;i<lv;i++)seeds[i]=RowSeed(i);}
    duckdb::unique_ptr<FunctionData>Copy()const override{
        auto cp=duckdb::make_uniq<SSVSBindData>(l,m,k);cp->seeds=seeds;return cp;}
    bool Equals(const FunctionData&o)const override{
        auto&x=o.Cast<SSVSBindData>();return l==x.l&&m==x.m&&k==x.k;}
};
struct SSVSOperation{
    template<class S>static void Initialize(S&s){s.sketch=nullptr;s.rand_state=RandomSeed();}
    template<class S>static void InitSketch(S&s,const SSVSBindData&b){
        if(!s.IsSet()) s.sketch=new SSVSSketch(b.l,b.m,b.k,&b.seeds);}
    template<class I,class S,class OP>static void Operation(S&s,const I&in,duckdb::AggregateUnaryInput&u){
        auto&b=u.input.bind_data->Cast<SSVSBindData>();InitSketch(s,b);
        s.sketch->Insert(FlowKey(in.GetData(),in.GetSize()),s.rand_state);}
    template<class I,class S,class OP>static void ConstantOperation(S&s,const I&in,duckdb::AggregateUnaryInput&u,duckdb::idx_t n){
        auto&b=u.input.bind_data->Cast<SSVSBindData>();InitSketch(s,b);
        FlowKey key(in.GetData(),in.GetSize());
        for(duckdb::idx_t i=0;i<n;i++) s.sketch->Insert(key,s.rand_state);}
    template<class S,class OP>static void Combine(const S&src,S&tgt,duckdb::AggregateInputData&){
        if(!src.IsSet())return;
        if(!tgt.IsSet()){tgt.sketch=new SSVSSketch(*src.sketch);return;}
        tgt.sketch->Merge(*const_cast<SSVSSketch*>(src.sketch));}
    template<class T,class S>static void Finalize(S&s,T&t,duckdb::AggregateFinalizeData&fd){
        if(!s.IsSet()){fd.ReturnNull();return;}
        // Compute the noise floor once here so it is stored in the sketch;
        // otherwise every ssvs_query() row recomputes it from 20,000 samples.
        s.sketch->computeNoise();
        auto b=s.sketch->Serialize();
        t=duckdb::StringVector::AddStringOrBlob(fd.result,b.data(),b.size());}
    template<class S>static void Destroy(S&s,duckdb::AggregateInputData&){delete s.sketch;s.sketch=nullptr;}
    static bool IgnoreNull(){return true;}
};

// Flow-key helpers (defined in sketches_aggregate.cpp). Applied when the functions
// are registered, they let every key-taking function accept the flow as any
// number of columns of any type, e.g. countmin_agg(src, dst) and
// countmin_query(sketch, src, dst). Merge functions are left unchanged.
duckdb::AggregateFunction AcceptAnyFlowKey(duckdb::AggregateFunction function);
duckdb::ScalarFunction AcceptAnyFlowKey(duckdb::ScalarFunction function);

// These are all defined in sketches_aggregate.cpp
duckdb::AggregateFunction GetCountMinAggregateFunction();
duckdb::AggregateFunction GetCUAggregateFunction();
duckdb::ScalarFunction GetCUQueryFunction();
duckdb::ScalarFunction GetCUUpdateFunction();
duckdb::ScalarFunction GetCUMergeFunction();
duckdb::ScalarFunction GetCountMinQueryFunction();
duckdb::ScalarFunction GetCountMinUpdateFunction();
duckdb::ScalarFunction GetCountMinMergeFunction();

duckdb::AggregateFunction GetCountSketchAggregateFunction();
duckdb::ScalarFunction GetCountSketchQueryFunction();
duckdb::ScalarFunction GetCountSketchUpdateFunction();
duckdb::ScalarFunction GetCountSketchMergeFunction();

duckdb::AggregateFunction GetCMSCAggregateFunction();
duckdb::ScalarFunction GetCMSCQueryFunction();
duckdb::ScalarFunction GetCMSCUpdateFunction();
duckdb::ScalarFunction GetCMSCMergeFunction();

duckdb::AggregateFunction GetCUSCAggregateFunction();
duckdb::ScalarFunction GetCUSCQueryFunction();
duckdb::ScalarFunction GetCUSCUpdateFunction();
duckdb::ScalarFunction GetCUSCMergeFunction();

duckdb::AggregateFunction GetCSSCAggregateFunction();
duckdb::ScalarFunction GetCSSCQueryFunction();
duckdb::ScalarFunction GetCSSCUpdateFunction();
duckdb::ScalarFunction GetCSSCMergeFunction();




duckdb::AggregateFunction GetCMACAggregateFunction();
duckdb::ScalarFunction GetCMACQueryFunction();
duckdb::ScalarFunction GetCMACUpdateFunction();
duckdb::ScalarFunction GetCMACMergeFunction();

duckdb::AggregateFunction GetCUACAggregateFunction();
duckdb::ScalarFunction GetCUACQueryFunction();
duckdb::ScalarFunction GetCUACUpdateFunction();
duckdb::ScalarFunction GetCUACMergeFunction();

duckdb::AggregateFunction GetCSACAggregateFunction();
duckdb::ScalarFunction GetCSACQueryFunction();
duckdb::ScalarFunction GetCSACUpdateFunction();
duckdb::ScalarFunction GetCSACMergeFunction();

duckdb::AggregateFunction GetRCSAggregateFunction();
duckdb::ScalarFunction GetRCSQueryFunction();
duckdb::ScalarFunction GetRCSUpdateFunction();
duckdb::ScalarFunction GetRCSMergeFunction();

duckdb::AggregateFunction GetRCSACAggregateFunction();
duckdb::ScalarFunction GetRCSACQueryFunction();
duckdb::ScalarFunction GetRCSACUpdateFunction();
duckdb::ScalarFunction GetRCSACMergeFunction();

duckdb::AggregateFunction GetSSVSAggregateFunction();
duckdb::ScalarFunction GetSSVSQueryFunction();
duckdb::ScalarFunction GetSSVSUpdateFunction();
duckdb::ScalarFunction GetSSVSMergeFunction();


// ─── Timed query functions ────────────────────────────────────────────────────
// xxx_query_timed(sketch, key...): each timed run hashes the key and queries it
// 10,000 times; 5 runs, median and fastest reported in ns per query, plus the
// normal estimate. Details above TimedQueryImpl in the .cpp.
duckdb::ScalarFunction GetCountMinTimedQueryFunction();
duckdb::ScalarFunction GetCUTimedQueryFunction();
duckdb::ScalarFunction GetCountSketchTimedQueryFunction();
duckdb::ScalarFunction GetCMSCTimedQueryFunction();
duckdb::ScalarFunction GetCUSCTimedQueryFunction();
duckdb::ScalarFunction GetCSSCTimedQueryFunction();
duckdb::ScalarFunction GetCMACTimedQueryFunction();
duckdb::ScalarFunction GetCUACTimedQueryFunction();
duckdb::ScalarFunction GetCSACTimedQueryFunction();
duckdb::ScalarFunction GetRCSTimedQueryFunction();
duckdb::ScalarFunction GetRCSACTimedQueryFunction();
duckdb::ScalarFunction GetSSVSTimedQueryFunction();


// Timed insert versions of each sketch: xxx_update_timed(sketch, key...).
// Each timed run hashes and inserts one flow 100,000 times into a copy of the
// sketch; 5 runs, median and fastest reported in ns per insert. The input
// sketch is returned unchanged. Details above TimedUpdateImpl in the .cpp.
duckdb::ScalarFunction GetCountMinTimedUpdateFunction();
duckdb::ScalarFunction GetCUTimedUpdateFunction();
duckdb::ScalarFunction GetCountSketchTimedUpdateFunction();
duckdb::ScalarFunction GetCMSCTimedUpdateFunction();
duckdb::ScalarFunction GetCUSCTimedUpdateFunction();
duckdb::ScalarFunction GetCSSCTimedUpdateFunction();
duckdb::ScalarFunction GetCMACTimedUpdateFunction();
duckdb::ScalarFunction GetCUACTimedUpdateFunction();
duckdb::ScalarFunction GetCSACTimedUpdateFunction();
duckdb::ScalarFunction GetRCSTimedUpdateFunction();
duckdb::ScalarFunction GetRCSACTimedUpdateFunction();
duckdb::ScalarFunction GetSSVSTimedUpdateFunction();

// Whole-trace timed aggregates: xxx_agg_timed(key... [, sizing constants]).
// Build the same sketch as xxx_agg() and time every insert over the table in
// order, like the paper. Returns STRUCT(sketch BLOB, insert_ns DOUBLE,
// packets BIGINT, total_ms DOUBLE). Details above TraceTimedAggregate in the .cpp.
duckdb::AggregateFunction GetCountMinTimedAggregateFunction();
duckdb::AggregateFunction GetCUTimedAggregateFunction();
duckdb::AggregateFunction GetCountSketchTimedAggregateFunction();
duckdb::AggregateFunction GetCMSCTimedAggregateFunction();
duckdb::AggregateFunction GetCUSCTimedAggregateFunction();
duckdb::AggregateFunction GetCSSCTimedAggregateFunction();
duckdb::AggregateFunction GetCMACTimedAggregateFunction();
duckdb::AggregateFunction GetCUACTimedAggregateFunction();
duckdb::AggregateFunction GetCSACTimedAggregateFunction();
duckdb::AggregateFunction GetRCSTimedAggregateFunction();
duckdb::AggregateFunction GetRCSACTimedAggregateFunction();
duckdb::AggregateFunction GetSSVSTimedAggregateFunction();

} // namespace duckdb_sketches