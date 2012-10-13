/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * A performance benchmark for the HashTable.
 */

#include <math.h>

#include "Common.h"
#include "Context.h"
#include "Cycles.h"
#include "HashTable.h"
#include "Memory.h"
#include "OptionParser.h"
#include "KeyUtil.h"

namespace RAMCloud {
namespace {

class TestObject {
  public:
    // We don't care about tables or string keys, so we'll assume table 0
    // and let our keys be 64-bit integers.
    explicit TestObject(uint64_t key)
        : key(key)
    {
    }

    uint64_t key;

    DISALLOW_COPY_AND_ASSIGN(TestObject); // NOLINT
} __attribute__((aligned(64)));

class TestObjectKeyComparer : public HashTable::KeyComparer {
  public:
    bool
    doesMatch(Key& key, uint64_t candidate)
    {
        // A pointer to a TestObject has been squished into each
        // uint64_t.
        TestObject* candidateObject = reinterpret_cast<TestObject*>(candidate);
        Key candidateKey(0, &candidateObject->key,
            sizeof(candidateObject->key));
        return (key == candidateKey);
    }
};

} // anonymous namespace

void
hashTableBenchmark(uint64_t nkeys, uint64_t nlines)
{
    uint64_t i;
    TestObjectKeyComparer keyComparer;
    HashTable ht(nlines, keyComparer);
    TestObject** values = new TestObject*[nkeys];

    printf("hash table keys: %lu\n", nkeys);
    printf("hash table lines: %lu\n", nlines);
    printf("cache line size: %d\n", ht.bytesPerCacheLine());
    printf("load factor: %.03f\n", static_cast<double>(nkeys) /
           (static_cast<double>(nlines) * ht.entriesPerCacheLine()));

    printf("populating table...");
    fflush(stdout);
    for (i = 0; i < nkeys; i++) {
        Key key(0, &i, sizeof(i));
        values[i] = new TestObject(i);
        uint64_t reference(reinterpret_cast<uint64_t>(values[i]));
        ht.replace(key, reference);

        // Here just in case.
        //   NB: This alters our PerfDistribution bin counts,
        //       so be sure to reset them below!
        uint64_t outReference = 0;
        assert(ht.lookup(key, outReference));
        assert(outReference == reference);
    }
    printf("done!\n");

    // replace/lookup affects the PerfDistribution, so reset for replace
    // benchmarks
    ht.resetPerfCounters();

    printf("running replace measurements...");
    fflush(stdout);

    // don't use a CycleCounter, as we may want to run without PERF_COUNTERS
    uint64_t replaceCycles = Cycles::rdtsc();
    for (i = 0; i < nkeys; i++) {
        Key key(0, &i, sizeof(i));
        uint64_t reference(reinterpret_cast<uint64_t>(values[i]));
        ht.replace(key, reference);
    }
    i = Cycles::rdtsc() - replaceCycles;
    printf("done!\n");

    delete[] values;
    values = NULL;

    const HashTable::PerfCounters & pc = ht.getPerfCounters();

    printf("== replace() ==\n");

    printf("    external avg: %lu ticks, %lu nsec\n",
           i / nkeys, Cycles::toNanoseconds(i / nkeys));

    printf("    internal avg: %lu ticks, %lu nsec\n",
           pc.replaceCycles / nkeys,
           Cycles::toNanoseconds(pc.replaceCycles / nkeys));

    printf("    multi-cacheline accesses: %lu / %lu\n",
           pc.insertChainsFollowed, nkeys);

    // replace affects the PerfDistribution, so reset for lookup benchmarks
    ht.resetPerfCounters();

    printf("running lookup measurements...");
    fflush(stdout);

    // don't use a CycleCounter, as we may want to run without PERF_COUNTERS
    uint64_t lookupCycles = Cycles::rdtsc();
    for (i = 0; i < nkeys; i++) {
        Key key(0, &i, sizeof(i));
        uint64_t reference = 0;
        bool success = ht.lookup(key, reference);
        assert(success);
        assert(reinterpret_cast<TestObject*>(reference)->key == i);
    }
    i = Cycles::rdtsc() - lookupCycles;
    printf("done!\n");

    printf("== lookup() ==\n");

    printf("    external avg: %lu ticks, %lu nsec\n", i / nkeys,
        Cycles::toNanoseconds(i / nkeys));

    printf("    internal avg: %lu ticks, %lu nsec\n",
           pc.lookupEntryCycles / nkeys,
           Cycles::toNanoseconds(pc.lookupEntryCycles / nkeys));

    printf("    multi-cacheline accesses: %lu / %lu\n",
           pc.lookupEntryChainsFollowed, nkeys);

    printf("    minikey false positives: %lu\n", pc.lookupEntryHashCollisions);

    printf("    min ticks: %lu, %lu nsec\n",
           pc.lookupEntryDist.getMin(),
           Cycles::toNanoseconds(pc.lookupEntryDist.getMin()));

    printf("    max ticks: %lu, %lu nsec\n",
           pc.lookupEntryDist.getMax(),
           Cycles::toNanoseconds(pc.lookupEntryDist.getMax()));

    uint64_t *histogram = static_cast<uint64_t *>(
        Memory::xmalloc(HERE, nlines * sizeof(histogram[0])));
    memset(histogram, 0, sizeof(nlines * sizeof(histogram[0])));

    for (i = 0; i < nlines; i++) {
        HashTable::CacheLine *cl;
        HashTable::Entry *entry;

        int depth = 1;
        cl = &ht.buckets.get()[i];
        entry = &cl->entries[ht.entriesPerCacheLine() - 1];
        while ((cl = entry->getChainPointer()) != NULL) {
            depth++;
            entry = &cl->entries[ht.entriesPerCacheLine() - 1];
        }
        histogram[depth]++;
    }

    // TODO(ongaro) Dump raw data instead for standard tools or scripts to use.

    printf("chaining histogram:\n");
    for (i = 0; i < nlines; i++) {
        if (histogram[i] != 0) {
            double percent = static_cast<double>(histogram[i]) * 100.0 /
                             static_cast<double>(nlines);
            printf("%5lu: %.4f%%\n", i, percent);
        }
    }

    free(histogram);
    histogram = NULL;

    printf("lookup cycle histogram:\n");
    printf("%s\n", pc.lookupEntryDist.toString().c_str());
}

} // namespace RAMCloud

int
main(int argc, char **argv)
{
    using namespace RAMCloud;

    Context context(true);

    uint64_t hashTableMegs, numberOfKeys;
    double loadFactor;

    OptionsDescription benchmarkOptions("HashTableBenchmark");
    benchmarkOptions.add_options()
        ("HashTableMegs,h",
         ProgramOptions::value<uint64_t>(&hashTableMegs)->
            default_value(1),
         "Megabytes of memory allocated to the HashTable")
        ("LoadFactor,f",
         ProgramOptions::value<double>(&loadFactor)->
            default_value(0.50),
         "Load factor desired (automatically calculate the number of keys)")
        ("NumberOfKeys,n",
         ProgramOptions::value<uint64_t>(&numberOfKeys)->
            default_value(0),
         "Number of keys to insert into the HashTable (overrides LoadFactor)");

    OptionParser optionParser(benchmarkOptions, argc, argv);

    uint64_t numberOfCachelines = (hashTableMegs * 1024 * 1024) /
        HashTable::bytesPerCacheLine();

    // If the user specified a load factor, auto-calculate the number of
    // keys based on the number of cachelines.
    if (numberOfKeys == 0) {
        uint64_t totalEntries = numberOfCachelines *
            HashTable::entriesPerCacheLine();
        numberOfKeys = static_cast<uint64_t>(loadFactor *
                          static_cast<double>(totalEntries));
    }

    hashTableBenchmark(numberOfKeys, numberOfCachelines);
    return 0;
}
