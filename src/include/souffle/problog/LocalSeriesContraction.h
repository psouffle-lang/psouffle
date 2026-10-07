#pragma once

#include "souffle/problog/PrivateFactorRewrite.h"

namespace souffle::problog {

// Compatibility entry point. The shared planner still fails closed on an
// incomplete view or unknown owner history; full-pipeline callers use the
// fused API to share preparation and a single commit with terminal factoring.
inline LocalSeriesContractionResult contractLocalSeries(WorkingDerivationGraph& graph,
        WorkingSubgraphView& view, bool completeDerivations = false) {
    auto result = rewritePrivateFactors(graph, view, true, false, completeDerivations);
    auto& stats = result.series.stats;
    stats.analysisMs = result.stats.preparationMs + result.stats.seriesPlanMs;
    stats.mutationMs = result.stats.mutationMs;
    stats.totalMs = result.stats.totalMs;
    return std::move(result.series);
}

}  // namespace souffle::problog
