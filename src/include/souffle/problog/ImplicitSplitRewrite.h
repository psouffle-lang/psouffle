#pragma once

#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/GraphAnalyzer.h"
#include "souffle/problog/GraphRewriter.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace souffle::problog {

enum class ImplicitSplitMode {
    None,
    Naive,
};

struct SplitNodeRef {
    NodePtr base;
    std::size_t alias = 0;

    bool isAlias() const {
        return alias != 0;
    }

    bool operator==(const SplitNodeRef& other) const {
        return base == other.base && alias == other.alias;
    }
};

struct SplitNodeRefHash {
    std::size_t operator()(const SplitNodeRef& ref) const {
        std::size_t h1 = std::hash<NodePtr>{}(ref.base);
        std::size_t h2 = std::hash<std::size_t>{}(ref.alias);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
    }
};

struct ImplicitSplitOverlayEdge {
    EdgePtr baseEdge;
    std::vector<SplitNodeRef> inputs;
    std::vector<bool> negations;
    NodePtr output;
    double probability = 1.0;
    bool deterministic = true;
    bool active = true;
    bool hasOwnedSupportTokens = false;
    const std::vector<SupportToken>* supportTokensView = nullptr;
    std::vector<SupportToken> supportTokens;
};

struct ImplicitSplitOverlayStats {
    std::size_t aliasesCreated = 0;
    std::size_t edgesAliased = 0;
    std::size_t allFactsRewrites = 0;
    std::size_t singleHyperedgeRewrites = 0;
    std::size_t linearTwoEdgeRewrites = 0;
    std::size_t parallelEdgeRewrites = 0;
    std::size_t fanOutConvergeRewrites = 0;
    std::size_t removedEdges = 0;
    std::size_t factOutputsFolded = 0;
    std::size_t splitFactsConsidered = 0;
    std::size_t splitFactsAliased = 0;
    std::size_t splitFactsCacheHits = 0;
    std::size_t splitNaiveReachabilityRuns = 0;
    std::size_t splitNaiveReachabilityVisited = 0;
    std::size_t splitNaiveCapSkips = 0;
    std::size_t rebuildIndexCount = 0;
    std::size_t fastPathIterations = 0;
    double splitNaiveMs = 0.0;
    double splitAliasApplyMs = 0.0;
    double rebuildIndexMs = 0.0;
    double fastPathDetectMs = 0.0;
    double fastPathSummarizeMs = 0.0;
    double fastPathSingleMs = 0.0;
    double fastPathLinearMs = 0.0;
    double fastPathParallelMs = 0.0;
    double fastPathFanOutMs = 0.0;
    double fastPathAllFactsMs = 0.0;
};

struct OverlayOutputProbability {
    NodePtr output;
    double probability = 0.0;
};

struct OverlayFactCommit {
    NodePtr node;
    bool isFact = false;
    double probability = 0.0;
    std::vector<SupportToken> supportTokens;
};

struct OverlayEdgeCommit {
    EdgePtr baseEdge;
    std::vector<SplitNodeRef> inputs;
    std::vector<bool> negations;
    NodePtr output;
    double probability = 1.0;
    bool deterministic = true;
    std::vector<SupportToken> supportTokens;
};

struct OverlayGraphStats {
    std::size_t activeEdges = 0;
    std::size_t activeAliases = 0;
    std::size_t activeAliasRefs = 0;
    std::size_t activeAliasedEdges = 0;
    std::size_t derivedFactOverrides = 0;
};

struct MaterializedImplicitSplitGraph {
    std::unique_ptr<WorkingDerivationGraph> graph;
    std::vector<NodePtr> outputs;
    std::unordered_set<NodePtr> liveNodes;
    std::unordered_set<EdgePtr> liveEdges;
    std::size_t aliasNodes = 0;
    std::size_t totalNodes = 0;
    std::size_t totalEdges = 0;
};

struct RewritePatternCounts {
    std::size_t singleHyperedge = 0;
    std::size_t linearTwoEdge = 0;
    std::size_t parallelEdge = 0;
    std::size_t allFactsToSO = 0;
    std::size_t fanOutConverge = 0;
    std::size_t general = 0;
    std::size_t unknown = 0;
};

struct ImplicitSplitPipelineOptions {
    ImplicitSplitMode splitMode = ImplicitSplitMode::Naive;
    bool runOverlayFastPaths = true;
    bool runOverlaySingleHyperedge = true;
    bool runOverlayLinearTwoEdge = true;
    bool runOverlayParallelEdge = true;
    bool runOverlayFanOutConverge = true;
    bool runOverlayAllFacts = true;
    bool runMaterializedGraphRewrite = true;
    bool computeOutputMarginals = false;
    bool collectPatternStats = false;
    bool iterateSplitRewrite = false;
    std::size_t maxOuterIterations = 32;
};

struct ImplicitSplitPipelineStats {
    ImplicitSplitOverlayStats overlayStats;
    std::size_t activeAliasRefs = 0;
    std::size_t activeAliasedEdges = 0;
    RewritePatternCounts materializedDetectedBefore;
    RewritePatternCounts materializedDetectedAfter;
    GraphRewriteStats graphRewriteStats;
    std::size_t outerIterations = 0;
    std::size_t materializedAliasNodes = 0;
    std::size_t materializedNodesBefore = 0;
    std::size_t materializedEdgesBefore = 0;
    std::size_t materializedNodesAfter = 0;
    std::size_t materializedEdgesAfter = 0;
    double overlayPrepMs = 0.0;
    double overlaySplitMs = 0.0;
    double overlayFastPathMs = 0.0;
    double materializeMs = 0.0;
    double graphDetectMs = 0.0;
    double graphRewriteMs = 0.0;
    double totalMs = 0.0;
};

struct ImplicitSplitPipelineResult {
    MaterializedImplicitSplitGraph materialized;
    std::vector<OverlayOutputProbability> outputProbabilities;
    std::vector<OverlayOutputProbability> directOutputs;
    std::vector<OverlayFactCommit> factCommits;
    std::vector<OverlayEdgeCommit> edgeCommits;
    std::vector<EdgePtr> inactiveBaseEdges;
    std::vector<std::pair<std::string, double>> carriedPrecomputedTupleProbs;
    bool needsResidualGraph = true;
    ImplicitSplitPipelineStats stats;
};

class ImplicitSplitOverlay {
public:
    explicit ImplicitSplitOverlay(const WorkingDerivationGraphViewInterface& view);

    bool applySplit(ImplicitSplitMode mode, ImplicitSplitOverlayStats* stats = nullptr);
    bool rewriteFastPathsToFixpoint(bool enableSingleHyperedge = true, bool enableLinearTwoEdge = true,
            bool enableParallelEdge = true, bool enableFanOutConverge = true, bool enableAllFacts = true,
            ImplicitSplitOverlayStats* stats = nullptr);

    std::vector<OverlayOutputProbability> collectDirectOutputProbabilities() const;
    std::vector<OverlayFactCommit> collectFactCommits() const;
    std::vector<OverlayEdgeCommit> collectEdgeCommits() const;
    std::vector<EdgePtr> collectInactiveBaseEdges() const;
    OverlayGraphStats computeStats() const;
    std::string summarize() const;
    MaterializedImplicitSplitGraph materializeToGraph(
            const std::unordered_set<NodePtr>* skippedOutputNodes = nullptr) const;

    const std::vector<NodePtr>& getOutputs() const {
        return outputs_;
    }

    const std::vector<ImplicitSplitOverlayEdge>& getEdges() const {
        return edges_;
    }

private:
    struct BaseNodeState {
        bool originalIsFact = false;
        bool currentIsFact = false;
        double factProbability = 0.0;
        bool needOutput = false;
        bool hasEvidence = false;
        bool hasOwnedFactSupportTokens = false;
        const std::vector<SupportToken>* factSupportTokensView = nullptr;
        std::vector<SupportToken> factSupportTokens;
    };

    const WorkingDerivationGraphViewInterface& view_;
    std::unordered_map<NodePtr, BaseNodeState> nodeState_;
    std::vector<NodePtr> outputs_;
    std::vector<ImplicitSplitOverlayEdge> edges_;
    std::unordered_map<NodePtr, std::size_t> nextAliasIdByFact_;
    std::unordered_map<NodePtr, std::vector<std::size_t>> aliasesByFact_;
    std::unordered_map<NodePtr, std::vector<std::size_t>> cachedBaseOutgoingEdgesByFact_;
    class SplitDirtyTracker {
    public:
        explicit SplitDirtyTracker(ImplicitSplitOverlay& overlay);

        void seedAllFacts();
        std::vector<NodePtr> takeFacts();
        void noteEdgeRemoval(const std::vector<SplitNodeRef>& oldInputs);
        void noteEdgeAddition(const std::vector<SplitNodeRef>& newInputs);
        void noteEdgeRewrite(const std::vector<SplitNodeRef>& oldInputs, const std::vector<SplitNodeRef>& newInputs);
        void noteFactifiedNode(const NodePtr& node);
        bool initialized() const;

    private:
        void markFact(const NodePtr& fact);
        void markInputs(const std::vector<SplitNodeRef>& inputs);

        ImplicitSplitOverlay& overlay_;
        std::vector<NodePtr> facts_;
        std::unordered_set<NodePtr> factSet_;
        bool initialized_ = false;
    };
    SplitDirtyTracker splitDirty_;
    mutable std::unordered_map<NodePtr, std::vector<std::size_t>> activeIncomingEdgeIdsByNode_;
    mutable std::unordered_map<SplitNodeRef, std::vector<std::size_t>, SplitNodeRefHash> activeOutgoingEdgeIdsByRef_;
    mutable std::unordered_map<NodePtr, std::size_t> activeSemanticInputOccurrencesByFact_;
    mutable std::vector<std::size_t> activeEdgeIds_;
    mutable bool activeEdgeIndicesDirty_ = false;
    std::size_t activeEdgeCount_ = 0;

    bool isFactRef(const SplitNodeRef& ref) const;
    double factProbabilityOf(const SplitNodeRef& ref) const;
    const std::vector<SupportToken>& factSupportTokensOf(const BaseNodeState& state) const;
    const std::vector<SupportToken>& factSupportTokensOf(const NodePtr& node) const;
    const std::vector<SupportToken>& edgeSupportTokensOf(const ImplicitSplitOverlayEdge& edge) const;
    void setFactSupportTokens(BaseNodeState& state, std::vector<SupportToken> tokens);
    void setEdgeSupportTokens(ImplicitSplitOverlayEdge& edge, std::vector<SupportToken> tokens);
    std::size_t activeIncomingCount(const NodePtr& node) const;
    std::size_t activeOutgoingCount(const SplitNodeRef& ref) const;
    std::size_t activeSemanticInputCount(const NodePtr& fact) const;
    const std::vector<std::size_t>& activeIncomingEdges(const NodePtr& node) const;
    const std::vector<std::size_t>& activeOutgoingEdges(const SplitNodeRef& ref) const;
    const std::vector<std::size_t>& activeOutgoingEdgesSnapshot(const SplitNodeRef& ref) const;
    bool canSplitFact(const NodePtr& fact) const;
    void markActiveEdgeIndicesDirty();
    void ensureActiveEdgeIndices() const;
    void rebuildActiveEdgeIndices() const;
    void factifyNode(const NodePtr& node, double probability, std::vector<SupportToken> supportTokens);
    void deactivateEdge(ImplicitSplitOverlayEdge& edge);
    void removeEdge(ImplicitSplitOverlayEdge& edge);
    void collapseEdgeToFact(ImplicitSplitOverlayEdge& edge, double probability, std::vector<SupportToken> supportTokens);
    void rewriteEdgeInPlace(ImplicitSplitOverlayEdge& edge, const std::vector<SplitNodeRef>& oldInputs,
            std::vector<SplitNodeRef> newInputs, std::vector<bool> newNegations, double probability,
            std::vector<SupportToken> supportTokens);
    void addSyntheticEdge(std::vector<SplitNodeRef> inputs, std::vector<bool> negations, const NodePtr& output,
            double probability, std::vector<SupportToken> supportTokens = {});
    void replaceEdgesWithSyntheticEdge(const std::vector<std::size_t>& removedEdgeIndices,
            std::vector<SplitNodeRef> inputs, std::vector<bool> negations, const NodePtr& output,
            double probability, std::vector<SupportToken> supportTokens = {});

    std::vector<std::vector<std::size_t>> partitionFactOutgoingEdgesNaive(
            const NodePtr& fact, ImplicitSplitOverlayStats* stats = nullptr) const;
    void applyEdgeGroupsAsAliases(const NodePtr& fact, const std::vector<std::vector<std::size_t>>& groups,
            ImplicitSplitOverlayStats* stats);

    struct SingleHyperedgeCandidate {
        std::size_t edgeIndex = 0;
        SplitNodeRef si{};
        bool siNegated = false;
        std::vector<std::size_t> affectedEdges;
    };

    struct AllFactsCandidate {
        std::size_t edgeIndex = 0;
    };

    struct FanOutConvergeInfo {
        std::vector<std::size_t> fanEdges;
        std::size_t convEdge = 0;
        bool fanNegated = false;
        bool mixedPolarity = false;
        NodePtr exit = nullptr;
        std::vector<SplitNodeRef> xiRefs;
    };

    struct FastPathCandidate {
        enum class Kind {
            LinearTwoEdge,
            ParallelEdge,
            FanOutConverge,
        };

        Kind kind;
        std::size_t edgeIndex = 0;
        std::size_t auxEdgeIndex = 0;
        std::vector<std::size_t> edgeIndices;
        SplitNodeRef entryRef{};
        bool primaryNegated = false;
        bool mixedPolarity = false;
        NodePtr output = nullptr;
        std::vector<SplitNodeRef> internalNodes;
    };

    struct GenericFastPathRoundPlan {
        std::vector<FastPathCandidate> selectedCandidates;

        bool empty() const {
            return selectedCandidates.empty();
        }
    };

    struct DirectLocalFastPathResult {
        bool changed = false;
        bool activeEdgesExhausted = false;
    };

    struct GenericFastPathRoundResult {
        bool changed = false;
        bool rebuiltIndicesBeforeLocal = false;
        DirectLocalFastPathResult local{};
    };

    struct FastPathScheduleOptions {
        bool enableSingleHyperedge = true;
        bool enableLinearTwoEdge = true;
        bool enableParallelEdge = true;
        bool enableFanOutConverge = true;
        bool enableAllFacts = true;

        bool enableDirectLocalPhase() const {
            return enableAllFacts || enableSingleHyperedge;
        }

        bool enableGenericRounds() const {
            return enableLinearTwoEdge || enableParallelEdge || enableFanOutConverge;
        }
    };

    bool buildAllFactsCandidate(std::size_t edgeIndex, AllFactsCandidate* candidate) const;
    bool applyAllFactsCandidate(const AllFactsCandidate& candidate, ImplicitSplitOverlayStats* stats);
    bool rewriteAllFactsPass(ImplicitSplitOverlayStats* stats);
    bool buildDirectSingleHyperedgeCandidate(std::size_t edgeIndex, SingleHyperedgeCandidate* candidate) const;
    bool applyDirectSingleHyperedgeCandidate(
            const SingleHyperedgeCandidate& candidate, ImplicitSplitOverlayStats* stats);
    bool rewriteSingleHyperedgePass(ImplicitSplitOverlayStats* stats);
    void ensureActiveEdgeIndicesWithStats(ImplicitSplitOverlayStats* stats);
    DirectLocalFastPathResult runDirectLocalFastPaths(
            const FastPathScheduleOptions& options, ImplicitSplitOverlayStats* stats);
    GenericFastPathRoundResult runGenericFastPathRound(
            const FastPathScheduleOptions& options, ImplicitSplitOverlayStats* stats);
    bool runGenericFastPathRounds(const FastPathScheduleOptions& options, ImplicitSplitOverlayStats* stats);
    bool classifyLinearTwoEdge(std::size_t edgeIndex, SplitNodeRef* entryOut, bool* entryNegatedOut,
            NodePtr* midOut, std::size_t* nextEdgeOut) const;
    bool classifyFanOutConverge(const SplitNodeRef& entryRef, FanOutConvergeInfo* outInfo) const;
    GenericFastPathRoundPlan buildGenericFastPathRoundPlan(
            const FastPathScheduleOptions& options, ImplicitSplitOverlayStats* stats) const;
    bool applyGenericFastPathRoundPlan(const GenericFastPathRoundPlan& plan, ImplicitSplitOverlayStats* stats);
    bool applyFastPathCandidate(const FastPathCandidate& candidate, ImplicitSplitOverlayStats* stats);
    std::vector<std::size_t> collectAffectedEdgesForSemanticFact(const NodePtr& fact) const;
    std::vector<std::size_t> collectAffectedEdgesForSingleHyperedgeRewrite(std::size_t edgeIndex) const;

    std::vector<SplitNodeRef> enumerateActiveFactRefs() const;
    std::vector<const ImplicitSplitOverlayEdge*> activeEdgesSorted() const;
};

std::vector<OverlayOutputProbability> computeGraphOutputMarginalsExact(
        const WorkingDerivationGraphViewInterface& view,
        const std::vector<NodePtr>& outputs,
        const std::unordered_map<NodePtr, double>* precomputedOutputs = nullptr);

std::string summarizeOverlayProbabilities(const std::vector<OverlayOutputProbability>& outputs);
RewritePatternCounts countRewritePatterns(const std::vector<SISORegionInfo>& regions);
std::string summarizeRewritePatternCounts(const RewritePatternCounts& counts);
ImplicitSplitPipelineResult runImplicitSplitRewritePipeline(
        const WorkingDerivationGraphViewInterface& view,
        const ImplicitSplitPipelineOptions& options = {});

}  // namespace souffle::problog
