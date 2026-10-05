
#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <climits>
#include <chrono>

#include "souffle/problog/DerivationGraph.h"

// Everything lives in one header, in namespace incra.
namespace incra {

// Shorthand aliases taken from the host DerivationGraph.h.
using NodePtr = std::shared_ptr<Node>;
using EdgePtr = std::shared_ptr<Hyperedge>;
using DGView  = IncrementalDerivationGraphViewInterface;

// A small helper to stringify a node and an edge consistently.
static inline std::string node_id(const NodePtr& n) {
    return n ? n->getTuple().toString() : std::string("<null>");
}
static inline std::string edge_id(const EdgePtr& e, DGView& view) {
    std::ostringstream oss;
    oss << "[";
    auto ins = view.getInputs(e);
    for (size_t i = 0; i < ins.size(); ++i) {
        if (i) oss << ", ";
        oss << node_id(ins[i]);
    }
    oss << " -> " << node_id(view.getOutput(e)) << "]";
    return oss.str();
}

static inline bool is_anchor_path_safe(const NodePtr& n) {
    // Treat evidence as output-equivalent for safety.
    return n && !n->isQueryNode() && !n->hasEvidence();
}

struct Region {
    std::unordered_set<NodePtr> nodes;
    std::unordered_set<EdgePtr> edges;
};

struct Boundaries {
    std::set<NodePtr> out_induced;   // region nodes whose outgoing edges received new inputs
    std::set<NodePtr> scope_induced; // region nodes that are least parents of delta scopes
    std::set<NodePtr> residual;      // remaining boundary nodes
};

struct IncRegionAnalysis {
    Region region;
    Boundaries boundaries;
    enum class AnchorKind : uint8_t { Edge, Node };
    struct AnchorCandidate {
        AnchorKind kind = AnchorKind::Edge;
        EdgePtr edge;
        NodePtr node;
        std::vector<NodePtr> pathNodes;
        std::vector<EdgePtr> pathEdges;
        static AnchorCandidate fromEdge(const EdgePtr& e) {
            AnchorCandidate c;
            c.kind = AnchorKind::Edge;
            c.edge = e;
            return c;
        }
        static AnchorCandidate fromNode(const NodePtr& n) {
            AnchorCandidate c;
            c.kind = AnchorKind::Node;
            c.node = n;
            return c;
        }
        static AnchorCandidate fromEdge(
                const EdgePtr& e, std::vector<NodePtr> nodes, std::vector<EdgePtr> edges) {
            AnchorCandidate c = fromEdge(e);
            c.pathNodes = std::move(nodes);
            c.pathEdges = std::move(edges);
            return c;
        }
        static AnchorCandidate fromNode(
                const NodePtr& n, std::vector<NodePtr> nodes, std::vector<EdgePtr> edges) {
            AnchorCandidate c = fromNode(n);
            c.pathNodes = std::move(nodes);
            c.pathEdges = std::move(edges);
            return c;
        }
    };
    std::unordered_map<NodePtr, std::vector<AnchorCandidate>> mergeableAnchorsByHead;
};

static inline std::string anchor_id(const IncRegionAnalysis::AnchorCandidate& a, DGView& view) {
    if (a.kind == IncRegionAnalysis::AnchorKind::Node) {
        return std::string("node:") + node_id(a.node);
    }
    return std::string("edge:") + edge_id(a.edge, view);
}

struct Stats {
    size_t optimized_recomputed = 0;  // #non-fact nodes to recompute under our region
    size_t naive_recomputed     = 0;  // #non-fact nodes in delta-reachable
    size_t dr_nodes             = 0;  // delta-reachable nodes
    size_t region_nodes         = 0;  // region nodes
    size_t dr_edges             = 0;  // delta-reachable edges
    size_t region_edges         = 0;  // region edges
    double ratio() const {
        return naive_recomputed == 0 ? 1.0 :
               double(optimized_recomputed) / double(naive_recomputed);
    }
    long   diff() const { return long(naive_recomputed) - long(optimized_recomputed); }
};

// The analyzer. All heavy lifting is here.
class RegionAnalyzer {
public:
    explicit RegionAnalyzer(DGView& view)
    : view_(view) {}

    // Main entry: run analysis and emit outputs.
    Stats analyze(const std::vector<NodePtr>& delta_input_facts,
                  const std::string& json_out_path = "",
                  const std::string& csv_out_path  = "") {
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        auto t0 = now();

        const bool verboseOutput = DerivationGraphViewInterface::isVerboseEnabled();
        const bool dumpStatsOutput = DerivationGraphViewInterface::isDumpStatsEnabled();
        const bool emitAnalysisConsole = incRegionalProfileEnabled || verboseOutput || dumpStatsOutput;
        const bool wantFullScopeOutput = dumpStatsOutput
                                        || !json_out_path.empty()
                                        || !csv_out_path.empty();
        const bool wantVerboseConsole = verboseOutput || dumpStatsOutput;

        // Reset caches per analysis run.
        resetDerivedCaches_();
        expandSnapshots_.clear();
        have_initial_snapshot_ = false;
        last_delta_inputs_.clear();
        last_delta_inputs_.insert(delta_input_facts.begin(), delta_input_facts.end());
        last_delta_nodes_ = view_.getDeltaInsertNodes();
        last_delta_nodes_.insert(last_delta_inputs_.begin(), last_delta_inputs_.end());
        last_delta_edges_.clear();
        auto deltaEdgeSet = view_.getDeltaInsertEdges();
        last_delta_edges_.insert(deltaEdgeSet.begin(), deltaEdgeSet.end());
        current_delta_sources_ = last_delta_inputs_;
        if (current_delta_sources_.empty()) {
            current_delta_sources_ = last_delta_nodes_;
        }
        reach_filter_ = reachFromCache_(current_delta_sources_);
        auto t1 = now();

        // Prepare structural maps used by lazy least-parent/scope computation.
        prepareGraphStructures_();
        auto t2 = now();

        // Build full "least-parents" and "scopes" only when required for debug/exports.
        if (wantFullScopeOutput) {
            buildLeastParents_();
            computeScopes_();
            debugPrintLeastParents_();
            debugPrintNodeScopes_();
            debugPrintEdgeScopes_();
        }

        // Initial region + boundaries + expansion
        Region region = initialRegion_(delta_input_facts);
        const size_t initial_nodes = region.nodes.size();
        const size_t initial_edges = region.edges.size();
        auto t3 = now();
        Boundaries B  = classifyBoundaries_(region);
        const size_t initial_boundary = B.out_induced.size() + B.scope_induced.size() + B.residual.size();
        if (incRegionalProfileEnabled) {
            initial_region_ = region;
            initial_boundaries_ = B;
            have_initial_snapshot_ = true;
        }
        auto t4 = now();
        std::vector<double> expand_iters_ms;
        std::vector<double> reexpand_iters_ms;
        const bool wantExpandIters = incRegionalProfileEnabled;
        expandToFixpoint_(region, B, "expand", wantExpandIters ? &expand_iters_ms : nullptr);
        const size_t after_expand_nodes = region.nodes.size();
        const size_t after_expand_edges = region.edges.size();
        const size_t after_expand_boundary = B.out_induced.size() + B.scope_induced.size() + B.residual.size();
        auto t5 = now();

        // Upstream closure: include ancestors (within reach_filter_) feeding region nodes,
        // then re-classify boundaries and re-run expansion to stabilize.
        double reclass_ms = 0.0;
        double reexpand_ms = 0.0;
        auto upstream_start = now();
        bool upstreamExpanded = upstreamClose_(region);
        auto upstream_end = now();
        double upstream_ms = toMs(upstream_end - t5);
        auto t7 = upstream_end;
        const size_t after_upstream_nodes = region.nodes.size();
        const size_t after_upstream_edges = region.edges.size();
        const size_t after_upstream_boundary = B.out_induced.size() + B.scope_induced.size() + B.residual.size();
        if (upstreamExpanded) {
            auto tr1 = now();
            B = classifyBoundaries_(region);
            auto tr2 = now();
            expandToFixpoint_(region, B, "reexpand", wantExpandIters ? &reexpand_iters_ms : nullptr);
            auto tr3 = now();
            reclass_ms = toMs(tr2 - tr1);
            reexpand_ms = toMs(tr3 - tr2);
            t7 = tr3;
        }
        const size_t after_reexpand_nodes = region.nodes.size();
        const size_t after_reexpand_edges = region.edges.size();
        const size_t after_reexpand_boundary = B.out_induced.size() + B.scope_induced.size() + B.residual.size();

        // Final safeguard: region must be a subset of delta-reachable. Reuse the
        // reachability filter computed at the start of this analysis run; the
        // view and delta sets are immutable within analyze().
        Region dr;
        dr.nodes = reach_filter_.nodes;
        dr.edges = reach_filter_.edges;
        auto t8 = now();
        intersectWithDeltaReachable_(region, dr);
        auto t9 = now();

        auto mergeAnchors = computeMergeableAnchors_(region, B);

        auto t10 = now();

        // Save last state for toDot()
        last_region_     = region;
        last_boundaries_ = B;
        dr_              = dr;
        last_analysis_.region = region;
        last_analysis_.boundaries = B;
        last_analysis_.mergeableAnchorsByHead = std::move(mergeAnchors);
        have_last_       = true;

        // Count stats
        Stats stats;
        stats.dr_nodes     = dr.nodes.size();
        stats.dr_edges     = dr.edges.size();
        stats.region_nodes = region.nodes.size();
        stats.region_edges = region.edges.size();
        const size_t total_nodes = view_.getNodes().size();
        const size_t total_edges = view_.getEdges().size();
        const double dr_edge_ratio = total_edges == 0 ? 0.0 :
                double(stats.dr_edges) / double(total_edges);
        for (const auto& n : region.nodes) if (!n->isFact) stats.optimized_recomputed++;
        for (const auto& n : dr.nodes)     if (!n->isFact) stats.naive_recomputed++;

        auto emitRelationHistogram = [&](const char* tag, const auto& nodes) {
            if (!incRegionalProfileEnabled || nodes.empty()) return;
            std::unordered_map<std::string, size_t> counts;
            counts.reserve(nodes.size());
            for (const auto& n : nodes) {
                if (!n) continue;
                counts[n->getTuple().relation_name]++;
            }
            std::vector<std::pair<std::string, size_t>> items(counts.begin(), counts.end());
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
            std::cout << "[inc-analyze-relations] tag=" << tag
                      << " total_nodes=" << nodes.size()
                      << " distinct_relations=" << items.size();
            const size_t limit = std::min<size_t>(items.size(), 12);
            for (size_t i = 0; i < limit; ++i) {
                std::cout << " " << items[i].first << "=" << items[i].second;
            }
            if (items.size() > limit) {
                std::cout << " ...";
            }
            std::cout << "\n";
        };

        // Emit
        auto joinMs = [](const std::vector<double>& vals) {
            if (vals.empty()) return std::string();
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(3);
            for (size_t i = 0; i < vals.size(); ++i) {
                if (i) oss << ",";
                oss << vals[i];
            }
            return oss.str();
        };
        if (emitAnalysisConsole) {
            std::cout << "[inc-analyze] timing(ms): "
                      << " reach=" << toMs(t1 - t0)
                      << " prepare=" << toMs(t2 - t1)
                      << " initRegion=" << toMs(t3 - t2)
                      << " classify=" << toMs(t4 - t3)
                      << " expand=" << toMs(t5 - t4)
                      << " upstreamClose=" << upstream_ms
                      << " reclassify=" << reclass_ms
                      << " reexpand=" << reexpand_ms
                      << " deltaReach=" << toMs(t8 - t7)
                      << " intersect=" << toMs(t9 - t8)
                      << " mergeable=" << toMs(t10 - t9)
                      << " deltaNodes=" << last_delta_nodes_.size()
                      << " regionNodes=" << stats.region_nodes
                      << " drNodes=" << stats.dr_nodes
                      << " regionEdges=" << stats.region_edges
                      << " drEdges=" << stats.dr_edges
                      << " totalNodes=" << total_nodes
                      << " totalEdges=" << total_edges
                      << " drEdgeRatio=" << dr_edge_ratio
                      << " lpSources=" << lp_set_.size()
                      << " scopeSources=" << node_scope_.size()
                      << " scopeNodesTotal=" << last_scope_nodes_total_
                      << " scopeNodesExtra=" << last_scope_nodes_extra_
                      << " scopeEdgesTotal=" << last_scope_edges_total_
                      << " scopeEdgesExtra=" << last_scope_edges_extra_
                      << " initNodes=" << initial_nodes
                      << " initEdges=" << initial_edges
                      << " initBoundary=" << initial_boundary
                      << " afterExpandNodes=" << after_expand_nodes
                      << " afterExpandEdges=" << after_expand_edges
                      << " afterExpandBoundary=" << after_expand_boundary
                      << " afterUpstreamNodes=" << after_upstream_nodes
                      << " afterUpstreamEdges=" << after_upstream_edges
                      << " afterUpstreamBoundary=" << after_upstream_boundary
                      << " afterReexpandNodes=" << after_reexpand_nodes
                      << " afterReexpandEdges=" << after_reexpand_edges
                      << " afterReexpandBoundary=" << after_reexpand_boundary
                      << " headsIndexed=" << head_scope_index_ready_.size()
                      << " total=" << toMs(t10 - t0);
            if (wantExpandIters && !expand_iters_ms.empty()) {
                std::cout << " expand_iters_ms=" << joinMs(expand_iters_ms);
            }
            if (wantExpandIters && !reexpand_iters_ms.empty()) {
                std::cout << " reexpand_iters_ms=" << joinMs(reexpand_iters_ms);
            }
            std::cout << "\n";
        }
        emitRelationHistogram("delta_insert_nodes", last_delta_nodes_);
        emitRelationHistogram("delta_reachable_nodes", dr.nodes);
        emitRelationHistogram("region_nodes", region.nodes);
        if (emitAnalysisConsole) {
            emitConsole_(stats, region, wantVerboseConsole);
        }
        if (!json_out_path.empty()) emitJSON_(stats, region, json_out_path);
        if (!csv_out_path.empty())  emitCSV_(stats, region, csv_out_path);
        return stats;
    }

    // toDot with visual marks: region nodes (double border), boundary nodes (orange),
    // mergeable node anchors (yellow), mergeable anchor paths (red), delta items (green).
    bool toDot(const std::string& path, bool mark_merge = true) {
        if (!have_last_) return false;
        std::ofstream out(path);
        if (!out) return false;
        out << "digraph G {\n";
        out << "  rankdir=LR;\n";

        auto inRegion = [&](const NodePtr& n){ return last_region_.nodes.count(n) > 0; };
        auto isOutB   = [&](const NodePtr& n){ return last_boundaries_.out_induced.count(n)   > 0; };
        auto isScopeB = [&](const NodePtr& n){ return last_boundaries_.scope_induced.count(n)> 0; };
        auto isResB   = [&](const NodePtr& n){ return last_boundaries_.residual.count(n)      > 0; };
        auto isDeltaN = [&](const NodePtr& n){
            return isIn_<NodePtr>(n, view_.getDeltaInsertNodes())
                || isIn_<NodePtr>(n, view_.getDeltaDeleteNodes());
        };
        auto isDeltaE = [&](const EdgePtr& e){
            return isIn_<EdgePtr>(e, view_.getDeltaInsertEdges())
                || isIn_<EdgePtr>(e, view_.getDeltaDeleteEdges());
        };
        std::unordered_set<NodePtr> anchorNodes;
        std::unordered_set<EdgePtr> anchorEdges;
        std::unordered_set<EdgePtr> anchorBridgeEdges;
        auto edgeHasInput = [&](const EdgePtr& e, const NodePtr& n) {
            if (!e || !n) return false;
            for (const auto& in : view_.getInputs(e)) {
                if (in == n) return true;
            }
            return false;
        };
        auto addBridgeEdgesToHead = [&](const NodePtr& head, const NodePtr& source) {
            if (!head || !source) return;
            for (const auto& e : view_.getIncomingEdges(head)) {
                if (e && e->isDeterministic() && edgeHasInput(e, source)) {
                    anchorBridgeEdges.insert(e);
                }
            }
        };
        for (const auto& [head, anchors] : last_analysis_.mergeableAnchorsByHead) {
            for (const auto& anchor : anchors) {
                if (anchor.kind == IncRegionAnalysis::AnchorKind::Node) {
                    if (anchor.node) {
                        anchorNodes.insert(anchor.node);
                        addBridgeEdgesToHead(head, anchor.node);
                    }
                } else if (anchor.edge) {
                    anchorEdges.insert(anchor.edge);
                    addBridgeEdgesToHead(head, view_.getOutput(anchor.edge));
                }
            }
        }

        // Nodes
        for (auto& n : view_.getValidNodes()) {
            // base vs derived
            std::string shape = n->isFact ? "box" : "ellipse";
            std::string pen   = inRegion(n) ? "2"  : "1";
            std::string color = inRegion(n) ? "#1f77b4" : "black"; // region nodes: blue border
            if (anchorNodes.count(n)) {
                color = "#f2c744"; // yellow for mergeable node anchors
                pen = "2";
            }
            if (isOutB(n) || isScopeB(n) || isResB(n)) {
                color = "#ff7f0e"; // orange for boundary nodes
                pen   = "2";
                shape = n->isFact ? "box" : "ellipse";
            }
            if (isDeltaN(n)) {
                color = "#2ca02c"; // green for delta
                pen   = "2";
            }
            out << "  \"" << node_id(n) << "\""
                << " [shape=" << shape << ", penwidth=" << pen << ", color=\"" << color << "\"];\n";
        }

        // Hyperedges are rendered with intermediate edge nodes, similar to DerivationGraph::dumpDot.
        for (auto& e : view_.getValidEdges()) {
            auto head  = view_.getOutput(e);
            auto ins   = view_.getInputs(e);
            bool inRegEdge = last_region_.edges.count(e) > 0;
            bool delta     = isDeltaE(e);
            bool directAnchor = anchorEdges.count(e) > 0;
            bool bridgeAnchor = anchorBridgeEdges.count(e) > 0;
            bool canMerge  = mark_merge && (directAnchor || bridgeAnchor);
            std::ostringstream edgeNodeName;
            edgeNodeName << "edge_node_" << e->getId();
            std::string color = "black";
            std::string pen   = "1";
            if (inRegEdge) { color = "#1f77b4"; pen = "2"; }          // in-region edges: blue
            if (canMerge) { color = "red"; pen = "2"; }               // mergeable anchor path: red
            if (delta) { color = "#2ca02c"; pen = "2"; }              // delta edges: green
            out << "  " << quote_(edgeNodeName.str())
                << " [shape=point, width=0.2, height=0.2, label=\"\", style=filled, color=\"" << color
                << "\", fillcolor=\"" << color << "\", penwidth=" << pen << "];\n";
            for (auto& t : ins) {
                out << "  \"" << node_id(t) << "\" -> " << quote_(edgeNodeName.str())
                    << " [color=\"" << color << "\", penwidth=" << pen << ", style=solid];\n";
            }
            out << "  " << quote_(edgeNodeName.str()) << " -> \"" << node_id(head) << "\""
                << " [color=\"" << color << "\", penwidth=" << pen << ", style=solid];\n";
        }

        emitLegend_(out, mark_merge);
        out << "}\n";
        return true;
    }

    bool toDotForAnalysis(const std::string& path, const IncRegionAnalysis& analysis, bool mark_merge = true) {
        if (!have_last_) return false;
        last_region_ = analysis.region;
        last_boundaries_ = analysis.boundaries;
        last_analysis_ = analysis;
        return toDot(path, mark_merge);
    }

    // Accessors for last run
    const Region&     lastRegion()    const { return last_region_; }
    const Boundaries& lastBoundaries()const { return last_boundaries_; }
    const Region&     lastDeltaReachable() const { return dr_; }
    const IncRegionAnalysis& getLastAnalysis() const { return last_analysis_; }
    Boundaries recomputeBoundaries(const Region& R) { return classifyBoundaries_(R); }
    std::unordered_map<NodePtr, std::vector<IncRegionAnalysis::AnchorCandidate>>
    recomputeAnchors(const Region& R, const Boundaries& B) {
        return computeMergeableAnchors_(R, B);
    }
    const std::unordered_set<NodePtr>& getScopeOwnersForHead(const NodePtr& head) {
        static const std::unordered_set<NodePtr> kEmpty;
        if (!head) return kEmpty;
        auto it = head_scope_owners_cache_.find(head);
        if (it != head_scope_owners_cache_.end()) return it->second;
        ensureHeadScopeIndex_(head);
        std::unordered_set<NodePtr> owners;
        const auto& sources = getBranchingSourcesForHead_(head);
        for (const auto& s : sources) {
            auto sit = node_scope_.find(s);
            if (sit != node_scope_.end() && sit->second.count(head)) {
                owners.insert(s);
            }
        }
        return head_scope_owners_cache_.emplace(head, std::move(owners)).first->second;
    }
    const std::set<NodePtr>& getScopeNodesForSource(const NodePtr& source) {
        static const std::set<NodePtr> kEmpty;
        if (!source) return kEmpty;
        ensureScope_(source);
        auto it = node_scope_.find(source);
        return it == node_scope_.end() ? kEmpty : it->second;
    }
    const std::set<EdgePtr>& getScopeEdgesForSource(const NodePtr& source) {
        static const std::set<EdgePtr> kEmpty;
        if (!source) return kEmpty;
        ensureScope_(source);
        auto it = edge_scope_by_source_.find(source);
        return it == edge_scope_by_source_.end() ? kEmpty : it->second;
    }
    bool expandRegionFromSources(Region& R, const std::unordered_set<NodePtr>& sources,
                                 const Region& reachFilter) {
        if (sources.empty()) return false;
        bool changed = false;
        std::queue<NodePtr> q;
        std::unordered_set<NodePtr> seen;
        for (const auto& n : sources) {
            if (!n) continue;
            if (!reachFilter.nodes.empty() && !reachFilter.nodes.count(n)) continue;
            if (R.nodes.insert(n).second) changed = true;
            if (seen.insert(n).second) q.push(n);
        }
        while (!q.empty()) {
            auto cur = q.front();
            q.pop();
            for (const auto& e : view_.getOutgoingEdges(cur)) {
                if (!e) continue;
                if (!reachFilter.edges.empty() && !reachFilter.edges.count(e)) continue;
                if (R.edges.insert(e).second) changed = true;
                auto h = view_.getOutput(e);
                if (!h) continue;
                if (!reachFilter.nodes.empty() && !reachFilter.nodes.count(h)) continue;
                if (R.nodes.insert(h).second) changed = true;
                if (seen.insert(h).second) q.push(h);
            }
        }
        return changed;
    }
    const std::vector<IncRegionAnalysis::AnchorCandidate>& getMergeableAnchors(NodePtr v) const {
        static const std::vector<IncRegionAnalysis::AnchorCandidate> kEmpty;
        auto it = last_analysis_.mergeableAnchorsByHead.find(v);
        return it == last_analysis_.mergeableAnchorsByHead.end() ? kEmpty : it->second;
    }

private:
    // ---- helpers ----

    template <class T, class SetT>
    static bool isIn_(const T& x, const SetT& s) {
        return s.find(x) != s.end();
    }

    static bool isBase_(const NodePtr& n) { return n->isFact; }

    void resetDerivedCaches_() {
        lp_set_.clear();
        node_scope_.clear();
        edge_scope_by_source_.clear();
        edge_scope_index_.clear();
        reachable_cache_.clear();
        full_reachable_cache_.clear();
        scc_reach_cache_.clear();
        scc_reach_nodes_cache_.clear();
        head_branching_sources_cache_.clear();
        head_scope_owners_cache_.clear();
        head_scope_index_ready_.clear();
        anchor_candidates_cache_.clear();
        delta_out_nodes_ready_ = false;
        delta_lp_union_ready_ = false;
        delta_out_nodes_cache_.clear();
        delta_lp_union_cache_.clear();
        scope_index_global_ready_ = false;
    }

    void prepareGraphStructures_() {
        incoming_edges_map_.clear();
        outgoing_edges_map_.clear();
        preds_.clear();
        succs_.clear();
        const bool useReachEdges = !reach_filter_.edges.empty();
        if (useReachEdges) {
            for (auto& n : reach_filter_.nodes) {
                incoming_edges_map_[n];
                outgoing_edges_map_[n];
            }
            for (auto& e : reach_filter_.edges) {
                auto head = view_.getOutput(e);
                if (!head) {
                    continue;
                }
                incoming_edges_map_[head];
                outgoing_edges_map_[head];
                incoming_edges_map_[head].push_back(e);
                auto ins = view_.getInputs(e);
                for (auto& t : ins) {
                    incoming_edges_map_[t];
                    outgoing_edges_map_[t];
                    outgoing_edges_map_[t].push_back(e);
                }
            }
        } else {
            for (auto& n : view_.getValidNodes()) {
                incoming_edges_map_[n];
                outgoing_edges_map_[n];
            }
            for (auto& e : view_.getValidEdges()) {
                auto head = view_.getOutput(e);
                incoming_edges_map_[head].push_back(e);
                auto ins = view_.getInputs(e);
                for (auto& t : ins) {
                    outgoing_edges_map_[t].push_back(e);
                }
            }
        }
        delta_insert_edges_cache_.clear();
        for (auto& e : view_.getDeltaInsertEdges()) {
            delta_insert_edges_cache_.insert(e);
        }
        delta_insert_nodes_cache_.clear();
        for (auto& n : view_.getDeltaInsertNodes()) {
            delta_insert_nodes_cache_.insert(n);
        }
    }

    // Build Least-Parents (LP) sets for nodes following the dominance-based definition.
    void buildLeastParents_() {
        scope_index_global_ready_ = false;
        using Clock = std::chrono::steady_clock;
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        auto t_total_start = Clock::now();
        auto t0 = Clock::now();
        if (incoming_edges_map_.empty() || outgoing_edges_map_.empty()) {
            prepareGraphStructures_();
        }
        double t_prepare = toMs(Clock::now() - t0);
        lp_set_.clear();
        reachable_cache_.clear();
        scc_reach_cache_.clear();
        scc_reach_nodes_cache_.clear();
        t0 = Clock::now();
        auto& scc = view_.getCycleDependencyGraph();
        double t_scc = toMs(Clock::now() - t0);
        double t_scc_reach = 0.0;
        double t_scc_nodes = 0.0;
        double t_reachable = 0.0;
        double t_branch = 0.0;
        double t_dominators = 0.0;
        double t_select = 0.0;
        size_t sources_total = 0;
        size_t sources_used = 0;
        size_t sources_skipped = 0;
        size_t branch_edges = 0;
        size_t branch_nodes = 0;
        size_t reachable_nodes = 0;
        for (auto& source : view_.getValidNodes()) {
            sources_total++;
            auto outIt = outgoing_edges_map_.find(source);
            if (outIt == outgoing_edges_map_.end() || outIt->second.size() < 2) {
                lp_set_[source] = {};
                sources_skipped++;
                continue;
            }
            sources_used++;
            auto tr = Clock::now();
            auto cit = scc.nodeToCycleIndex.find(source);
            if (cit != scc.nodeToCycleIndex.end()) {
                auto ts = Clock::now();
                const auto& reachable = getReachNodes_(cit->second);
                t_scc_nodes += toMs(Clock::now() - ts);
                reachable_cache_[source] = reachable;
            } else {
                reachable_cache_[source] = forwardReachable_(source);
            }
            const auto& reachable = reachable_cache_[source];
            t_reachable += toMs(Clock::now() - tr);
            reachable_nodes += reachable.size();
            std::unordered_map<NodePtr, size_t> branch_counts;
            for (auto& edge : outIt->second) {
                if (!edge) continue;
                auto child = view_.getOutput(edge);
                if (!child) continue;
                auto tb = Clock::now();
                auto branchReach = forwardReachableFromBranch_(child, source);
                t_branch += toMs(Clock::now() - tb);
                branch_edges++;
                branch_nodes += branchReach.size();
                for (auto& node : branchReach) {
                    if (node.get() == source.get()) continue;
                    branch_counts[node]++;
                }
            }
            std::set<NodePtr> merge_nodes;
            for (auto& [node, count] : branch_counts) {
                if (count >= 2 && reachable.count(node)) {
                    merge_nodes.insert(node);
                }
            }
            if (merge_nodes.empty()) {
                lp_set_[source] = {};
                continue;
            }
            auto td = Clock::now();
            auto dom = computeDominators_(source, reachable);
            t_dominators += toMs(Clock::now() - td);
            auto ts = Clock::now();
            std::set<NodePtr> least;
            for (auto& m : merge_nodes) {
                bool dominated = false;
                for (auto& other : merge_nodes) {
                    if (m.get() == other.get()) continue;
                    auto dit = dom.find(m);
                    if (dit != dom.end() && dit->second.count(other)) {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated) least.insert(m);
            }
            t_select += toMs(Clock::now() - ts);
            lp_set_[source] = std::move(least);
        }
        double t_total = toMs(Clock::now() - t_total_start);
        if (incRegionalProfileEnabled || DerivationGraphViewInterface::isVerboseEnabled() ||
                DerivationGraphViewInterface::isDumpStatsEnabled()) {
            std::cout << "[least-parents] timing(ms): prepare=" << t_prepare
                      << " scc=" << t_scc
                      << " sccReach=" << t_scc_reach /*kept for compatibility*/
                      << " reachNodes=" << t_scc_nodes
                      << " reachable=" << t_reachable
                      << " branch=" << t_branch
                      << " dominators=" << t_dominators
                      << " select=" << t_select
                      << " total=" << t_total
                      << " sources=" << sources_total
                      << " used=" << sources_used
                      << " skipped=" << sources_skipped
                      << " branches=" << branch_edges
                      << " branchNodes=" << branch_nodes
                      << " reachNodes=" << reachable_nodes
                      << "\n";
        }
    }

    void computeScopes_() {
        scope_index_global_ready_ = false;
        node_scope_.clear();
        edge_scope_by_source_.clear();
        edge_scope_index_.clear();
        for (auto& n : view_.getValidNodes()) {
            std::set<NodePtr> scope_nodes;
            std::set<EdgePtr> scope_edges;
            auto lp_it = lp_set_.find(n);
            auto reach_it = reachable_cache_.find(n);
            if (lp_it != lp_set_.end() && reach_it != reachable_cache_.end()) {
                for (auto& m : lp_it->second) {
                    collectScopeFrom_(m, reach_it->second, scope_nodes, scope_edges);
                }
            }
            node_scope_[n] = scope_nodes;
            edge_scope_by_source_[n] = scope_edges;
            for (auto& e : scope_edges) {
                edge_scope_index_[e].insert(n);
            }
        }
        scope_index_global_ready_ = true;
    }

    const std::set<NodePtr>& edgeScope_(const EdgePtr& e, bool ignore_insert_edges=false) {
        static const std::set<NodePtr> kEmpty;
        auto it = edge_scope_index_.find(e);
        return it == edge_scope_index_.end() ? kEmpty : it->second;
    }

    const std::set<NodePtr>& getPreds_(const NodePtr& node) {
        static const std::set<NodePtr> kEmpty;
        if (!node) return kEmpty;
        auto it = preds_.find(node);
        if (it != preds_.end()) return it->second;
        std::set<NodePtr> preds;
        auto inIt = incoming_edges_map_.find(node);
        if (inIt != incoming_edges_map_.end()) {
            for (auto& e : inIt->second) {
                auto ins = view_.getInputs(e);
                for (auto& t : ins) {
                    if (t) preds.insert(t);
                }
            }
        }
        return preds_.emplace(node, std::move(preds)).first->second;
    }

    const std::set<NodePtr>& getSuccs_(const NodePtr& node) {
        static const std::set<NodePtr> kEmpty;
        if (!node) return kEmpty;
        auto it = succs_.find(node);
        if (it != succs_.end()) return it->second;
        std::set<NodePtr> succs;
        auto outIt = outgoing_edges_map_.find(node);
        if (outIt != outgoing_edges_map_.end()) {
            for (auto& e : outIt->second) {
                auto head = view_.getOutput(e);
                if (head) succs.insert(head);
            }
        }
        return succs_.emplace(node, std::move(succs)).first->second;
    }

    std::set<NodePtr> forwardReachable_(const NodePtr& source) {
        std::set<NodePtr> reachable;
        if (!source) return reachable;
        std::vector<NodePtr> stack;
        stack.push_back(source);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (!reachable.insert(cur).second) continue;
            const auto& succs = getSuccs_(cur);
            if (succs.empty()) continue;
            for (auto& next : succs) {
                stack.push_back(next);
            }
        }
        return reachable;
    }

    std::unordered_map<NodePtr, std::set<NodePtr>> computeDominators_(const NodePtr& source,
            const std::set<NodePtr>& reachable) {
        std::unordered_map<NodePtr, std::set<NodePtr>> dom;
        if (!source) return dom;
        for (auto& node : reachable) {
            if (node.get() == source.get()) {
                dom[node] = { source };
            } else {
                dom[node] = reachable;
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& node : reachable) {
                if (node.get() == source.get()) continue;
                std::set<NodePtr> intersection;
                bool first = true;
                bool has_pred = false;
                const auto& preds = getPreds_(node);
                for (auto& pred : preds) {
                    if (!reachable.count(pred)) continue;
                    has_pred = true;
                    if (first) {
                        intersection = dom[pred];
                        first = false;
                    } else {
                        std::set<NodePtr> temp;
                        std::set_intersection(intersection.begin(), intersection.end(),
                            dom[pred].begin(), dom[pred].end(), std::inserter(temp, temp.begin()));
                        intersection.swap(temp);
                    }
                }
                std::set<NodePtr> new_dom;
                new_dom.insert(node);
                if (has_pred) {
                    new_dom.insert(intersection.begin(), intersection.end());
                }
                if (dom[node] != new_dom) {
                    dom[node] = std::move(new_dom);
                    changed = true;
                }
            }
        }
        return dom;
    }

    void collectScopeFrom_(const NodePtr& target, const std::set<NodePtr>& reachable,
            std::set<NodePtr>& scope_nodes, std::set<EdgePtr>& scope_edges) {
        if (!target) return;
        std::vector<NodePtr> stack;
        std::unordered_set<NodePtr> visited;
        stack.push_back(target);
        visited.insert(target);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (!reachable.count(cur)) continue;
            scope_nodes.insert(cur);
            auto it = incoming_edges_map_.find(cur);
            if (it == incoming_edges_map_.end()) continue;
            for (auto& e : it->second) {
                auto ins = view_.getInputs(e);
                bool all_reachable = true;
                for (auto& tail : ins) {
                    if (!reachable.count(tail)) {
                        all_reachable = false;
                        break;
                    }
                }
                if (!all_reachable) continue;
                scope_edges.insert(e);
                for (auto& tail : ins) {
                    if (visited.insert(tail).second) {
                        stack.push_back(tail);
                    }
                }
            }
        }
    }

    struct ReachInfo {
        std::unordered_set<NodePtr> nodes;
        std::unordered_set<EdgePtr> edges;
    };

    ReachInfo reachFromCache_(const std::set<NodePtr>& sources) {
        ReachInfo info;
        const auto& cacheNodes = view_.getDeltaInsertReachableNodes();
        const auto& cacheEdges = view_.getDeltaInsertReachableEdges();
        if (!cacheNodes.empty() || !cacheEdges.empty()) {
            info.nodes.insert(cacheNodes.begin(), cacheNodes.end());
            info.edges.insert(cacheEdges.begin(), cacheEdges.end());
            return info;
        }
        std::vector<NodePtr> srcVec;
        srcVec.reserve(sources.size());
        for (const auto& n : sources) {
            srcVec.push_back(n);
        }
        Region dr = deltaReachable_(srcVec);
        info.nodes.insert(dr.nodes.begin(), dr.nodes.end());
        info.edges.insert(dr.edges.begin(), dr.edges.end());
        return info;
    }

    std::set<NodePtr> forwardReachableFromBranch_(const NodePtr& start, const NodePtr& source) {
        std::set<NodePtr> reachable;
        if (!start) return reachable;
        std::vector<NodePtr> stack;
        stack.push_back(start);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (cur.get() == source.get()) continue;
            if (!reachable.insert(cur).second) continue;
            const auto& succs = getSuccs_(cur);
            if (succs.empty()) continue;
            for (auto& next : succs) {
                stack.push_back(next);
            }
        }
        return reachable;
    }

    bool mergeableEdgeAtHead_(const NodePtr& head, const EdgePtr& edge) {
        if (!head || !edge) return false;
        if (edge->isDeterministic()) return false;
        if (delta_insert_edges_cache_.count(edge)) return false;
        ensureHeadScopeIndex_(head);
        if (!edgeRespectsScopes_(head, edge)) return false;
        if (!edgeNonSubsumed_(head, edge)) return false;
        return true;
    }

    std::pair<bool, std::string> edgeAnchorStatus_(const NodePtr& head, const EdgePtr& edge) {
        if (!head || !edge) return {false, "null_edge_or_head"};
        if (edge->isDeterministic()) return {false, "edge_deterministic"};
        if (delta_insert_edges_cache_.count(edge)) return {false, "edge_delta_insert"};
        ensureHeadScopeIndex_(head);
        if (!edgeRespectsScopes_(head, edge)) return {false, "edge_scope_fail"};
        if (!edgeNonSubsumed_(head, edge)) return {false, "edge_subsumed"};
        return {true, "ok"};
    }

    void logAnchorCheck_(const NodePtr& head, const std::string& cand, bool ok, const std::string& reason) {
        if (!incRegionalProfileEnabled) return;
        std::cout << "[inc-regional] anchor-check head=" << node_id(head)
                  << " candidate=" << cand
                  << " result=" << (ok ? "OK" : "NO")
                  << " reason=" << reason << "\n";
    }

    // Diagnostic helper to explain why a boundary head has no mergeable anchors.
    std::string explainMissingAnchor_(const NodePtr& head) {
        if (!head) return "null_head";
        const auto& inEdges = view_.getIncomingEdges(head);
        if (inEdges.empty()) {
            return "no_incoming_edges";
        }
        bool hasNonDet = false;
        bool hasNonDeltaNonDet = false;
        bool anyScopeFail = false;
        bool anySubsumedFail = false;
        bool anyMergeable = false;
        size_t detCount = 0;
        size_t deltaCount = 0;
        for (const auto& e : inEdges) {
            if (!e) continue;
            if (e->isDeterministic()) {
                detCount++;
                continue;
            }
            hasNonDet = true;
            if (delta_insert_edges_cache_.count(e)) {
                deltaCount++;
                continue;
            }
            hasNonDeltaNonDet = true;
            ensureHeadScopeIndex_(head);
            if (!edgeRespectsScopes_(head, e)) {
                anyScopeFail = true;
                continue;
            }
            if (!edgeNonSubsumed_(head, e)) {
                anySubsumedFail = true;
                continue;
            }
            anyMergeable = true;
        }
        if (anyMergeable) {
            return "mergeable_present";
        }
        if (!hasNonDet) {
            return "all_incoming_edges_deterministic";
        }
        if (!hasNonDeltaNonDet) {
            return "only_delta_insert_edges";
        }
        if (anyScopeFail) {
            return "scope_constraint";
        }
        if (anySubsumedFail) {
            return "subsumed_by_other_inputs";
        }
        return "unknown";
    }

    bool edgeRespectsScopes_(const NodePtr& head, const EdgePtr& edge) {
        auto it = edge_scope_index_.find(edge);
        if (it == edge_scope_index_.end()) return true;
        for (auto& source : it->second) {
            auto lp_it = lp_set_.find(source);
            if (lp_it == lp_set_.end()) return false;
            if (!lp_it->second.count(head)) return false;
        }
        return true;
    }

    bool edgeNonSubsumed_(const NodePtr& head, const EdgePtr& edge) {
        auto inputs = view_.getInputs(edge);
        std::set<NodePtr> candidate(inputs.begin(), inputs.end());
        auto inEdges = view_.getIncomingEdges(head);
        for (auto& other : inEdges) {
            if (other.get() == edge.get()) continue;
            auto otherInputs = view_.getInputs(other);
            std::set<NodePtr> otherSet(otherInputs.begin(), otherInputs.end());
            if (otherSet.empty()) continue;
            if (std::includes(candidate.begin(), candidate.end(), otherSet.begin(), otherSet.end())) {
                return false;
            }
        }
        return true;
    }

    // ---- region building ----

    Region initialRegion_(const std::vector<NodePtr>& delta_input_facts) {
        Region R;
        std::set<NodePtr> delta_nodes = view_.getDeltaInsertNodes();
        std::set<EdgePtr> delta_edges = view_.getDeltaInsertEdges();
        for (auto& n : delta_input_facts) delta_nodes.insert(n);

        std::unordered_set<NodePtr> out_nodes;
        for (auto& e : delta_edges) {
            out_nodes.insert(view_.getOutput(e));
        }

        std::unordered_set<NodePtr> scope_nodes_union;
        std::unordered_set<EdgePtr> scope_edges_union;
        for (auto& x : delta_nodes) {
            if (!reach_filter_.nodes.empty() && !reach_filter_.nodes.count(x)) {
                continue;
            }
            auto oit = outgoing_edges_map_.find(x);
            if (oit == outgoing_edges_map_.end() || oit->second.size() < 2) {
                continue;
            }
            ensureScope_(x);
            auto nit = node_scope_.find(x);
            if (nit != node_scope_.end()) {
                scope_nodes_union.insert(nit->second.begin(), nit->second.end());
            }
            auto eit = edge_scope_by_source_.find(x);
            if (eit != edge_scope_by_source_.end()) {
                scope_edges_union.insert(eit->second.begin(), eit->second.end());
            }
        }

        std::unordered_set<NodePtr> candidate_nodes(delta_nodes.begin(), delta_nodes.end());
        candidate_nodes.insert(out_nodes.begin(), out_nodes.end());
        candidate_nodes.insert(scope_nodes_union.begin(), scope_nodes_union.end());

        std::unordered_set<EdgePtr> candidate_edges(delta_edges.begin(), delta_edges.end());
        candidate_edges.insert(scope_edges_union.begin(), scope_edges_union.end());

        // Track scope-only additions (filtered by reachability).
        last_scope_nodes_total_ = 0;
        last_scope_nodes_extra_ = 0;
        for (auto& n : scope_nodes_union) {
            if (!reach_filter_.nodes.count(n)) continue;
            last_scope_nodes_total_++;
            if (!delta_nodes.count(n) && !out_nodes.count(n)) {
                last_scope_nodes_extra_++;
            }
        }
        last_scope_edges_total_ = 0;
        last_scope_edges_extra_ = 0;
        for (auto& e : scope_edges_union) {
            if (!reach_filter_.edges.count(e)) continue;
            last_scope_edges_total_++;
            if (!delta_edges.count(e)) {
                last_scope_edges_extra_++;
            }
        }

        for (auto& n : candidate_nodes) {
            if (reach_filter_.nodes.count(n)) {
                R.nodes.insert(n);
            }
        }
        for (auto& e : candidate_edges) {
            if (reach_filter_.edges.count(e)) {
                R.edges.insert(e);
            }
        }
        return R;
    }

    Boundaries classifyBoundaries_(const Region& R) {
        Boundaries B;
        std::unordered_set<NodePtr> boundary_nodes;
        if (!outgoing_edges_map_.empty()) {
            for (auto& n : R.nodes) {
                auto it = outgoing_edges_map_.find(n);
                if (it == outgoing_edges_map_.end()) continue;
                for (auto& e : it->second) {
                    auto head = view_.getOutput(e);
                    if (!head || R.nodes.count(head)) continue;
                    boundary_nodes.insert(n);
                    break;
                }
            }
        } else {
            for (auto& e : view_.getValidEdges()) {
                auto head = view_.getOutput(e);
                auto ins = view_.getInputs(e);
                if (R.nodes.count(head)) continue;
                for (auto& tail : ins) {
                    if (R.nodes.count(tail)) {
                        boundary_nodes.insert(tail);
                    }
                }
            }
        }
        if (boundary_nodes.empty()) {
            return B;
        }

        ensureDeltaOutNodes_();

        ensureDeltaLpUnion_();

        for (auto& n : boundary_nodes) {
            if (delta_out_nodes_cache_.count(n)) {
                B.out_induced.insert(n);
            } else if (delta_lp_union_cache_.count(n)) {
                B.scope_induced.insert(n);
            } else {
                B.residual.insert(n);
            }
        }
        return B;
    }

    bool edgeMergeable_(const EdgePtr& e) {
        auto head = view_.getOutput(e);
        return mergeableEdgeAtHead_(head, e);
    }

    enum class AnchorSearchStrategy : uint8_t {
        Local,
        DeterministicChain,
    };

    AnchorSearchStrategy anchorSearchStrategy_() const {
        const char* value = std::getenv("SOUFFLE_INC_REGION_ANCHOR_STRATEGY");
        if (value && std::strcmp(value, "local") == 0) {
            return AnchorSearchStrategy::Local;
        }
        return AnchorSearchStrategy::DeterministicChain;
    }

    static constexpr size_t kAnchorSearchMaxDepth_ = 4;
    static constexpr size_t kAnchorSearchNodeBudget_ = 64;

    void appendDeterministicPathStep_(
            const EdgePtr& edge,
            const NodePtr& excludedOutput,
            std::vector<NodePtr>& pathNodes,
            std::vector<EdgePtr>& pathEdges) {
        if (!edge) return;
        pathEdges.push_back(edge);
        NodePtr out = view_.getOutput(edge);
        if (out && out != excludedOutput) {
            pathNodes.push_back(out);
        }
        for (const auto& input : view_.getInputs(edge)) {
            if (input) {
                pathNodes.push_back(input);
            }
        }
    }

    static void appendAnchorCandidate_(
            std::vector<IncRegionAnalysis::AnchorCandidate>& anchors,
            std::unordered_set<NodePtr>& seenNodes,
            std::unordered_set<EdgePtr>& seenEdges,
            const IncRegionAnalysis::AnchorCandidate& candidate) {
        if (candidate.kind == IncRegionAnalysis::AnchorKind::Node) {
            if (candidate.node && seenNodes.insert(candidate.node).second) {
                anchors.push_back(candidate);
            }
            return;
        }
        if (candidate.edge && seenEdges.insert(candidate.edge).second) {
            anchors.push_back(candidate);
        }
    }

    void collectDeterministicInputAnchors_(
            const NodePtr& start,
            std::vector<IncRegionAnalysis::AnchorCandidate>& anchors,
            std::unordered_set<NodePtr>& seenNodeAnchors,
            std::unordered_set<EdgePtr>& seenEdgeAnchors,
            AnchorSearchStrategy strategy,
            std::vector<NodePtr> initialPathNodes,
            std::vector<EdgePtr> initialPathEdges) {
        using AnchorCandidate = IncRegionAnalysis::AnchorCandidate;
        if (!start) return;

        struct Item {
            NodePtr node;
            size_t depth = 0;
            std::vector<NodePtr> pathNodes;
            std::vector<EdgePtr> pathEdges;
        };
        std::queue<Item> q;
        std::unordered_set<NodePtr> seenSearch;
        q.push({start, 0, std::move(initialPathNodes), std::move(initialPathEdges)});
        seenSearch.insert(start);
        size_t visited = 0;

        while (!q.empty() && visited++ < kAnchorSearchNodeBudget_) {
            Item item = q.front();
            q.pop();
            NodePtr cur = item.node;
            if (!cur || !is_anchor_path_safe(cur)) {
                continue;
            }
            if (cur->isFact) {
                if (cur->getProbability() < 1.0 && !delta_insert_nodes_cache_.count(cur)) {
                    appendAnchorCandidate_(
                            anchors, seenNodeAnchors, seenEdgeAnchors,
                            AnchorCandidate::fromNode(cur, item.pathNodes, item.pathEdges));
                }
                continue;
            }
            if (delta_insert_nodes_cache_.count(cur)) {
                continue;
            }
            for (const auto& inEdge : view_.getIncomingEdges(cur)) {
                if (!inEdge) continue;
                if (!inEdge->isDeterministic()) {
                    if (!delta_insert_edges_cache_.count(inEdge)) {
                        auto pathNodes = item.pathNodes;
                        pathNodes.push_back(cur);
                        appendAnchorCandidate_(
                                anchors, seenNodeAnchors, seenEdgeAnchors,
                                AnchorCandidate::fromEdge(inEdge, std::move(pathNodes), item.pathEdges));
                    }
                    continue;
                }
                if (delta_insert_edges_cache_.count(inEdge)) {
                    continue;
                }
                if (strategy == AnchorSearchStrategy::Local) {
                    continue;
                }
                if (item.depth >= kAnchorSearchMaxDepth_) {
                    continue;
                }
                auto nextPathNodes = item.pathNodes;
                auto nextPathEdges = item.pathEdges;
                appendDeterministicPathStep_(inEdge, nullptr, nextPathNodes, nextPathEdges);
                for (const auto& tail : view_.getInputs(inEdge)) {
                    if (tail && seenSearch.insert(tail).second) {
                        q.push({tail, item.depth + 1, nextPathNodes, nextPathEdges});
                    }
                }
            }
        }
    }

    const std::vector<IncRegionAnalysis::AnchorCandidate>& getAnchorCandidates_(const NodePtr& head) {
        using AnchorCandidate = IncRegionAnalysis::AnchorCandidate;
        static const std::vector<AnchorCandidate> kEmpty;
        if (!head) return kEmpty;
        auto it = anchor_candidates_cache_.find(head);
        if (it != anchor_candidates_cache_.end()) return it->second;
        if (delta_insert_nodes_cache_.count(head)) {
            return anchor_candidates_cache_.emplace(head, kEmpty).first->second;
        }
        std::vector<AnchorCandidate> anchors;
        std::unordered_set<NodePtr> seenNodeAnchors;
        std::unordered_set<EdgePtr> seenEdgeAnchors;
        const AnchorSearchStrategy strategy = anchorSearchStrategy_();
        const auto& inEs = view_.getIncomingEdges(head);
        for (const auto& e : inEs) {
            if (edgeAnchorStatus_(head, e).first) {
                appendAnchorCandidate_(
                        anchors, seenNodeAnchors, seenEdgeAnchors, AnchorCandidate::fromEdge(e));
                continue;
            }
            if (!e || !e->isDeterministic()) {
                continue;
            }
            std::vector<NodePtr> initialPathNodes;
            std::vector<EdgePtr> initialPathEdges;
            appendDeterministicPathStep_(e, head, initialPathNodes, initialPathEdges);
            const auto& ins = view_.getInputs(e);
            for (const auto& inNode : ins) {
                collectDeterministicInputAnchors_(
                        inNode, anchors, seenNodeAnchors, seenEdgeAnchors, strategy,
                        initialPathNodes, initialPathEdges);
            }
        }
        return anchor_candidates_cache_.emplace(head, std::move(anchors)).first->second;
    }

    const std::unordered_set<NodePtr>& incomingClosure_(
            const Region& R,
            const NodePtr& head,
            std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& cache) {
        static const std::unordered_set<NodePtr> kEmpty;
        if (!head) return kEmpty;
        auto it = cache.find(head);
        if (it != cache.end()) return it->second;
        std::unordered_set<NodePtr> closure;
        if (!R.nodes.count(head)) {
            return cache.emplace(head, std::move(closure)).first->second;
        }
        std::vector<NodePtr> stack;
        closure.insert(head);
        stack.push_back(head);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            const auto& inEdges = view_.getIncomingEdges(cur);
            for (const auto& e : inEdges) {
                if (!e) continue;
                for (const auto& in : view_.getInputs(e)) {
                    if (!in) continue;
                    if (!R.nodes.count(in)) continue;
                    if (closure.insert(in).second) {
                        stack.push_back(in);
                    }
                }
            }
        }
        return cache.emplace(head, std::move(closure)).first->second;
    }

    bool anchorBypassesBoundary_(
            const Region& R,
            const NodePtr& head,
            const IncRegionAnalysis::AnchorCandidate& candidate,
            std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& incomingClosureCache) {
        NodePtr source;
        if (candidate.kind == IncRegionAnalysis::AnchorKind::Node) {
            source = candidate.node;
        } else if (candidate.edge) {
            source = view_.getOutput(candidate.edge);
        }
        if (!source) {
            return true;
        }
        const auto& closure = incomingClosure_(R, head, incomingClosureCache);
        if (closure.empty()) {
            return true;
        }
        const auto& reachable = ensureFullReachable_(source);
        for (const auto& n : reachable) {
            if (!R.nodes.count(n)) continue;
            if (!closure.count(n)) {
                return true;
            }
        }
        return false;
    }

    bool anchorCandidateUsable_(
            const Region& R,
            const NodePtr& head,
            const IncRegionAnalysis::AnchorCandidate& candidate,
            std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& incomingClosureCache,
            std::string* reason = nullptr) {
        if (candidate.kind == IncRegionAnalysis::AnchorKind::Node) {
            if (!candidate.node) {
                if (reason) *reason = "node_null";
                return false;
            }
            if (delta_insert_nodes_cache_.count(candidate.node)) {
                if (reason) *reason = "input_node_delta_insert";
                return false;
            }
            if (R.nodes.count(candidate.node)) {
                if (reason) *reason = "input_node_in_region";
                return false;
            }
        } else {
            if (!candidate.edge) {
                if (reason) *reason = "edge_null";
                return false;
            }
            if (delta_insert_edges_cache_.count(candidate.edge)) {
                if (reason) *reason = "edge_anchor_delta_insert";
                return false;
            }
            if (R.edges.count(candidate.edge)) {
                if (reason) *reason = "edge_anchor_in_region";
                return false;
            }
        }
        for (const auto& node : candidate.pathNodes) {
            if (!node) continue;
            if (delta_insert_nodes_cache_.count(node)) {
                if (reason) *reason = "path_node_delta_insert";
                return false;
            }
            if (R.nodes.count(node)) {
                if (reason) *reason = "path_node_in_region";
                return false;
            }
        }
        for (const auto& edge : candidate.pathEdges) {
            if (!edge) continue;
            if (delta_insert_edges_cache_.count(edge)) {
                if (reason) *reason = "path_edge_delta_insert";
                return false;
            }
            if (R.edges.count(edge)) {
                if (reason) *reason = "path_edge_in_region";
                return false;
            }
        }
        if (anchorBypassesBoundary_(R, head, candidate, incomingClosureCache)) {
            if (reason) *reason = "anchor_bypass_incoming_closure";
            return false;
        }
        if (reason) *reason = "anchor_ok";
        return true;
    }

    bool mergeableHead_(const NodePtr& h, const Region& R) {
        if (!h || !R.nodes.count(h)) return false;
        if (delta_insert_nodes_cache_.count(h)) return false;
        std::unordered_map<NodePtr, std::unordered_set<NodePtr>> incomingClosureCache;
        for (const auto& candidate : getAnchorCandidates_(h)) {
            if (anchorCandidateUsable_(R, h, candidate, incomingClosureCache)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_map<NodePtr, std::vector<IncRegionAnalysis::AnchorCandidate>>
    computeMergeableAnchors_(const Region& R, const Boundaries& B) {
        using AnchorCandidate = IncRegionAnalysis::AnchorCandidate;
        std::unordered_map<NodePtr, std::vector<AnchorCandidate>> anchors;
        std::set<NodePtr> boundary_nodes;
        boundary_nodes.insert(B.out_induced.begin(), B.out_induced.end());
        boundary_nodes.insert(B.scope_induced.begin(), B.scope_induced.end());
        boundary_nodes.insert(B.residual.begin(), B.residual.end());
        std::vector<NodePtr> boundary_vec(boundary_nodes.begin(), boundary_nodes.end());
        if (incRegionalProfileEnabled) {
            std::sort(boundary_vec.begin(), boundary_vec.end(),
                    [](const NodePtr& a, const NodePtr& b) { return node_id(a) < node_id(b); });
        }
        if (incoming_edges_map_.empty() || outgoing_edges_map_.empty()) {
            prepareGraphStructures_();
        }
        std::unordered_map<NodePtr, std::unordered_set<NodePtr>> incoming_closure_cache;
        incoming_closure_cache.reserve(boundary_vec.size());
        for (const auto& head : boundary_vec) {
            if (delta_insert_nodes_cache_.count(head)) {
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] boundary head skip anchors: "
                              << node_id(head) << " reason=boundary_head_is_delta\n";
                }
                continue;
            }
            const auto& candidates = getAnchorCandidates_(head);
            for (const auto& cand : candidates) {
                std::string reason;
                if (!anchorCandidateUsable_(R, head, cand, incoming_closure_cache, &reason)) {
                    logAnchorCheck_(head, anchor_id(cand, view_), false, reason);
                    continue;
                }
                anchors[head].push_back(cand);
                logAnchorCheck_(head, anchor_id(cand, view_), true, reason);
            }
            if (incRegionalProfileEnabled) {
                const auto inCount = view_.getIncomingEdges(head).size();
                auto it = anchors.find(head);
                if (it == anchors.end() || it->second.empty()) {
                    std::cout << "[inc-regional] boundary head missing anchor: "
                              << node_id(head) << " reason=" << explainMissingAnchor_(head)
                              << " incoming_edges=" << inCount << "\n";
                } else {
                    std::cout << "[inc-regional] boundary head anchors: " << node_id(head)
                              << " anchors=";
                    std::vector<std::string> anchorStrs;
                    anchorStrs.reserve(it->second.size());
                    for (const auto& a : it->second) {
                        anchorStrs.push_back(anchor_id(a, view_));
                    }
                    std::sort(anchorStrs.begin(), anchorStrs.end());
                    for (size_t i = 0; i < anchorStrs.size(); ++i) {
                        if (i) std::cout << "; ";
                        std::cout << anchorStrs[i];
                    }
                    std::cout << "\n";
                }
            }
        }
        return anchors;
    }

    // Upstream closure: include ancestors (limited by reach_filter_) feeding any region node.
    bool upstreamClose_(Region& R) {
        bool changed = false;
        std::queue<NodePtr> q;
        for (auto n : R.nodes) q.push(n);
        std::unordered_set<NodePtr> seen(R.nodes.begin(), R.nodes.end());
        auto inFilterNode = [&](NodePtr n) {
            return reach_filter_.nodes.empty() || reach_filter_.nodes.count(n);
        };
        auto inFilterEdge = [&](EdgePtr e) {
            return reach_filter_.edges.empty() || reach_filter_.edges.count(e);
        };
        while (!q.empty()) {
            NodePtr cur = q.front();
            q.pop();
            for (auto e : view_.getIncomingEdges(cur)) {
                if (!inFilterEdge(e)) continue;
                if (R.edges.insert(e).second) changed = true;
                for (auto in : view_.getInputs(e)) {
                    if (!inFilterNode(in)) continue;
                    if (seen.insert(in).second) {
                        R.nodes.insert(in);
                        changed = true;
                        q.push(in);
                    }
                }
            }
        }
        return changed;
    }

    void expandToFixpoint_(Region& R, Boundaries& B, const char* phase,
                           std::vector<double>* iter_ms_out) {
        const bool verbose = incRegionalProfileEnabled;
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        auto boundaryCount = [](const Boundaries& b) {
            return b.out_induced.size() + b.scope_induced.size() + b.residual.size();
        };
        int guard = 0;
        auto recordSnapshot = [&](const char* reason, size_t boundary_before, size_t blocking_count,
                                   bool extended, double iter_ms) {
            if (!incRegionalProfileEnabled) return;
            ExpandSnapshot snap;
            snap.phase = phase;
            snap.iter = guard;
            snap.iterMs = iter_ms;
            snap.boundaryBefore = boundary_before;
            snap.boundaryAfter = boundaryCount(B);
            snap.blockingCount = blocking_count;
            snap.extended = extended;
            snap.reason = reason;
            snap.region = R;
            snap.boundaries = B;
            expandSnapshots_.push_back(std::move(snap));
        };
        while (guard++ < 10000) {
            auto iter_start = now();
            auto boundary_nodes = B.out_induced;
            boundary_nodes.insert(B.scope_induced.begin(), B.scope_induced.end());
            boundary_nodes.insert(B.residual.begin(), B.residual.end());
            const size_t boundary_count = boundary_nodes.size();
            if (verbose) {
                std::cout << "[region] expand iter " << guard
                          << " |R_nodes|=" << R.nodes.size()
                          << " |R_edges|=" << R.edges.size()
                          << " |boundary|=" << boundary_count
                          << std::endl;
            }
            if (boundary_nodes.empty()) {
                auto iter_end = now();
                const double iter_ms = toMs(iter_end - iter_start);
                if (iter_ms_out) iter_ms_out->push_back(iter_ms);
                recordSnapshot("boundary_empty", boundary_count, 0, false, iter_ms);
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-analyze-expand] phase=" << phase
                              << " iter=" << guard
                              << " ms=" << iter_ms
                              << " boundary=" << boundary_count
                              << " blocking=0 extended=0 reason=boundary_empty"
                              << " nodes=" << R.nodes.size()
                              << " edges=" << R.edges.size() << "\n";
                }
                if (verbose) {
                    std::cout << "[region] boundary empty, stopping expansion\n";
                }
                break;
            }
            std::vector<NodePtr> blocking;
            std::unordered_map<NodePtr, std::unordered_set<NodePtr>> scopeOwnersByBoundary;
            for (auto& n : boundary_nodes) {
                bool inScope = false;
                if (n) {
                    const auto& owners = getScopeOwnersForHead(n);
                    if (!owners.empty()) {
                        inScope = true;
                        scopeOwnersByBoundary.emplace(n, owners);
                    }
                }
                if (inScope || !mergeableHead_(n, R)) {
                    blocking.push_back(n);
                }
            }
            const size_t blocking_count = blocking.size();
            if (verbose) {
                std::cout << "[region] blocking boundary count=" << blocking_count << std::endl;
            }
            if (incRegionalProfileEnabled) {
                std::cout << "[region] anchor diagnostics begin (iter " << guard << ")\n";
                (void)computeMergeableAnchors_(R, B);
                std::cout << "[region] anchor diagnostics end (iter " << guard << ")\n";
            }
            if (blocking.empty()) {
                auto iter_end = now();
                const double iter_ms = toMs(iter_end - iter_start);
                if (iter_ms_out) iter_ms_out->push_back(iter_ms);
                recordSnapshot("all_mergeable", boundary_count, blocking_count, false, iter_ms);
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-analyze-expand] phase=" << phase
                              << " iter=" << guard
                              << " ms=" << iter_ms
                              << " boundary=" << boundary_count
                              << " blocking=" << blocking_count
                              << " extended=0 reason=all_mergeable"
                              << " nodes=" << R.nodes.size()
                              << " edges=" << R.edges.size() << "\n";
                }
                if (verbose) {
                    std::cout << "[region] all boundary nodes mergeable, stopping expansion\n";
                }
                break;
            }
            bool extended = false;
            for (auto& n : blocking) {
                bool scopeEmpty = true;
                auto ownersIt = scopeOwnersByBoundary.find(n);
                if (ownersIt != scopeOwnersByBoundary.end()) {
                    for (const auto& owner : ownersIt->second) {
                        ensureScope_(owner);
                        auto sit = node_scope_.find(owner);
                        if (sit != node_scope_.end()) {
                            if (!sit->second.empty()) {
                                scopeEmpty = false;
                            }
                            for (auto& sn : sit->second) {
                                if (reach_filter_.nodes.count(sn) && R.nodes.insert(sn).second) {
                                    extended = true;
                                }
                            }
                        }
                        auto eit = edge_scope_by_source_.find(owner);
                        if (eit != edge_scope_by_source_.end()) {
                            if (!eit->second.empty()) {
                                scopeEmpty = false;
                            }
                            for (auto& e : eit->second) {
                                if (reach_filter_.edges.count(e) && R.edges.insert(e).second) {
                                    extended = true;
                                }
                            }
                        }
                    }
                } else {
                    ensureScope_(n);
                    auto sit = node_scope_.find(n);
                    if (sit != node_scope_.end()) {
                        if (!sit->second.empty()) {
                            scopeEmpty = false;
                        }
                        for (auto& sn : sit->second) {
                            if (reach_filter_.nodes.count(sn) && R.nodes.insert(sn).second) {
                                extended = true;
                            }
                        }
                    }
                    auto eit = edge_scope_by_source_.find(n);
                    if (eit != edge_scope_by_source_.end()) {
                        if (!eit->second.empty()) {
                            scopeEmpty = false;
                        }
                        for (auto& e : eit->second) {
                            if (reach_filter_.edges.count(e) && R.edges.insert(e).second) {
                                extended = true;
                            }
                        }
                    }
                }
                if (scopeEmpty) {
                    auto oit = outgoing_edges_map_.find(n);
                    if (oit != outgoing_edges_map_.end()) {
                        for (const auto& e : oit->second) {
                            if (!e) continue;
                            if (reach_filter_.edges.count(e) && R.edges.insert(e).second) {
                                extended = true;
                            }
                            auto h = view_.getOutput(e);
                            if (h && reach_filter_.nodes.count(h) && R.nodes.insert(h).second) {
                                extended = true;
                            }
                        }
                    }
                }
            }
            if (!extended) {
                auto iter_end = now();
                const double iter_ms = toMs(iter_end - iter_start);
                if (iter_ms_out) iter_ms_out->push_back(iter_ms);
                recordSnapshot("no_extension", boundary_count, blocking_count, false, iter_ms);
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-analyze-expand] phase=" << phase
                              << " iter=" << guard
                              << " ms=" << iter_ms
                              << " boundary=" << boundary_count
                              << " blocking=" << blocking_count
                              << " extended=0 reason=no_extension"
                              << " nodes=" << R.nodes.size()
                              << " edges=" << R.edges.size() << "\n";
                }
                if (verbose) {
                    std::cout << "[region] scopes added no new items, stopping expansion\n";
                }
                break;
            }
            B = classifyBoundaries_(R);
            auto iter_end = now();
            const double iter_ms = toMs(iter_end - iter_start);
            if (iter_ms_out) iter_ms_out->push_back(iter_ms);
            recordSnapshot("continue", boundary_count, blocking_count, true, iter_ms);
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-analyze-expand] phase=" << phase
                          << " iter=" << guard
                          << " ms=" << iter_ms
                          << " boundary=" << boundary_count
                          << " blocking=" << blocking_count
                          << " extended=1 reason=continue"
                          << " nodes=" << R.nodes.size()
                          << " edges=" << R.edges.size() << "\n";
            }
        }
        if (guard >= 10000) {
            if (verbose) {
                std::cout << "[region] expand reached iteration guard limit" << std::endl;
            }
        }
        if (verbose) {
            std::cout << "[region] final region nodes=" << R.nodes.size()
                      << " edges=" << R.edges.size()
                      << " boundaries(out=" << B.out_induced.size()
                      << ", scope=" << B.scope_induced.size()
                      << ", residual=" << B.residual.size() << ")\n";
        }
    }

    // Compute delta-reachable region using impacted maps rebuilt after pruning.
    // This is insertion-only and reuses the precomputed reachability for delta inserts.
    Region deltaReachable_(const std::vector<NodePtr>& /*delta_input_facts*/) {
        Region DR;
        const auto& reachNodes = view_.getDeltaInsertReachableNodes();
        const auto& reachEdges = view_.getDeltaInsertReachableEdges();
        if (!reachNodes.empty() || !reachEdges.empty()) {
            DR.nodes.insert(reachNodes.begin(), reachNodes.end());
            DR.edges.insert(reachEdges.begin(), reachEdges.end());
        } else {
            const auto& nodeImpacted = view_.getNodeImpactedByDeltaInsert();
            const auto& edgeImpacted = view_.getEdgeImpactedByDeltaInsert();
            if (!nodeImpacted.empty() || !edgeImpacted.empty()) {
                for (const auto& kv : nodeImpacted) {
                    DR.nodes.insert(kv.first);
                    DR.nodes.insert(kv.second.begin(), kv.second.end());
                }
                for (const auto& kv : edgeImpacted) {
                    DR.edges.insert(kv.second.begin(), kv.second.end());
                }
            } else {
                const auto& liveNodes = view_.getNodes();
                const auto& liveEdges = view_.getEdges();
                std::queue<NodePtr> q;
                auto seed = [&](const NodePtr& src) {
                    if (!src || !liveNodes.count(src)) {
                        return;
                    }
                    if (DR.nodes.insert(src).second) {
                        q.push(src);
                    }
                };
                for (const auto& n : view_.getDeltaInsertNodes()) {
                    seed(n);
                }
                for (const auto& e : view_.getDeltaInsertEdges()) {
                    if (auto h = view_.getOutput(e)) {
                        seed(h);
                    }
                }
                while (!q.empty()) {
                    NodePtr cur = q.front();
                    q.pop();
                    for (const auto& e : cur->getOutgoingEdges()) {
                        if (!liveEdges.count(e)) {
                            continue;
                        }
                        DR.edges.insert(e);
                        NodePtr nxt = e->getOutput();
                        if (nxt && liveNodes.count(nxt) && DR.nodes.insert(nxt).second) {
                            q.push(nxt);
                        }
                    }
                }
            }
        }
        for (const auto& n : view_.getDeltaInsertNodes()) {
            DR.nodes.insert(n);
        }
        for (const auto& e : view_.getDeltaInsertEdges()) {
            DR.edges.insert(e);
            if (auto h = view_.getOutput(e)) {
                DR.nodes.insert(h);
            }
        }
        return DR;
    }

    void intersectWithDeltaReachable_(Region& R, const Region& DR) {
        // filter nodes
        for (auto it = R.nodes.begin(); it != R.nodes.end(); ) {
            if (!DR.nodes.count(*it)) it = R.nodes.erase(it);
            else ++it;
        }
        for (auto it = R.edges.begin(); it != R.edges.end(); ) {
            if (!DR.edges.count(*it)) it = R.edges.erase(it);
            else ++it;
        }
    }

    // ---- emissions ----

    void emitConsole_(const Stats& s, const Region& R, bool verbose) {
        std::cout << "=== Incremental Region Analysis ===\n";
        std::cout << "Delta nodes: " << last_delta_nodes_.size() << "\n";
        std::cout << "Region nodes: " << s.region_nodes << " / DR nodes: " << s.dr_nodes << "\n";
        std::cout << "Region edges: " << s.region_edges << " / DR edges: " << s.dr_edges << "\n";
        std::cout << "Recompute (optimized): " << s.optimized_recomputed
                  << " ; (naive): " << s.naive_recomputed
                  << " ; ratio: " << std::fixed << std::setprecision(3) << s.ratio()
                  << " ; diff: " << s.diff() << "\n";

        if (!verbose) return;

        // Per-node quick print (expensive; gated by dump-stats)
        for (auto& n : view_.getValidNodes()) {
            bool inR   = R.nodes.count(n);
            bool delta = view_.getDeltaInsertNodes().count(n) || view_.getDeltaDeleteNodes().count(n);
            std::cout << "node,\"" << node_id(n) << "\"," << (n->isFact?1:0) << "," << (inR?1:0) << "," << (delta?1:0)
                      << "," << 0 /*mergeable as node*/ << ",\"" << scopeStr_(node_scope_[n]) << "\"\n";
        }
        for (auto& e : view_.getValidEdges()) {
            bool inR   = R.edges.count(e);
            bool delta = view_.getDeltaInsertEdges().count(e) || view_.getDeltaDeleteEdges().count(e);
            std::cout << "edge,\"" << edge_id(e, view_) << "\"," << 0 << "," << (inR?1:0) << "," << (delta?1:0)
                      << "," << (edgeMergeable_(e)?1:0) << ",\"" << scopeStr_(edgeScope_(e)) << "\"\n";
        }
    }

    void emitJSON_(const Stats& s, const Region& R, const std::string& path) {
        std::ofstream out(path);
        if (!out) return;
        out << "{\n";
        out << "  \"stats\": {\"region_nodes\": " << s.region_nodes << ", \"dr_nodes\": " << s.dr_nodes
            << ", \"region_edges\": " << s.region_edges << ", \"dr_edges\": " << s.dr_edges
            << ", \"optimized\": " << s.optimized_recomputed << ", \"naive\": " << s.naive_recomputed
            << ", \"ratio\": " << std::fixed << std::setprecision(6) << s.ratio()
            << ", \"diff\": " << s.diff() << "},\n";
        out << "  \"nodes\": [\n";
        bool first = true;
        for (auto& n : view_.getValidNodes()) {
            if (!first) out << ",\n";
            first = false;
            bool inR   = R.nodes.count(n);
            bool delta = view_.getDeltaInsertNodes().count(n) || view_.getDeltaDeleteNodes().count(n);
            out << "    {\"id\": " << quote_(node_id(n)) << ", \"is_fact\": " << (n->isFact?1:0)
                << ", \"in_region\": " << (inR?1:0) << ", \"is_delta\": " << (delta?1:0)
                << ", \"scope\": " << quote_(scopeStr_(node_scope_[n])) << "}";
        }
        out << "\n  ],\n  \"edges\": [\n";
        first = true;
        for (auto& e : view_.getValidEdges()) {
            if (!first) out << ",\n";
            first = false;
            bool inR   = R.edges.count(e);
            bool delta = view_.getDeltaInsertEdges().count(e) || view_.getDeltaDeleteEdges().count(e);
            out << "    {\"id\": " << quote_(edge_id(e, view_)) << ", \"in_region\": " << (inR?1:0)
                << ", \"is_delta\": " << (delta?1:0) << ", \"mergeable\": " << (edgeMergeable_(e)?1:0)
                << ", \"scope\": " << quote_(scopeStr_(edgeScope_(e))) << "}";
        }
        out << "\n  ]\n}\n";
    }

    void emitCSV_(const Stats& s, const Region& R, const std::string& path) {
        std::ofstream out(path);
        if (!out) return;
        out << "type,id,is_fact,in_region,is_delta,mergeable,scope\n";
        for (auto& n : view_.getValidNodes()) {
            bool inR   = R.nodes.count(n);
            bool delta = view_.getDeltaInsertNodes().count(n) || view_.getDeltaDeleteNodes().count(n);
            out << "node," << quote_(node_id(n)) << "," << (n->isFact?1:0) << "," << (inR?1:0) << "," << (delta?1:0)
                << ",0," << quote_(scopeStr_(node_scope_[n])) << "\n";
        }
        for (auto& e : view_.getValidEdges()) {
            bool inR   = R.edges.count(e);
            bool delta = view_.getDeltaInsertEdges().count(e) || view_.getDeltaDeleteEdges().count(e);
            out << "edge," << quote_(edge_id(e, view_)) << ",0," << (inR?1:0) << "," << (delta?1:0)
                << "," << (edgeMergeable_(e)?1:0) << "," << quote_(scopeStr_(edgeScope_(e))) << "\n";
        }
    }

    void emitText_(const Stats& s, const Region& R, const Boundaries& B, const std::string& path) {
        std::ofstream out(path);
        if (!out) return;

        auto sortNodes = [&](const auto& nodes) {
            std::vector<NodePtr> v(nodes.begin(), nodes.end());
            std::sort(v.begin(), v.end(),
                    [](const NodePtr& a, const NodePtr& b) { return node_id(a) < node_id(b); });
            return v;
        };
        auto sortEdges = [&](const auto& edges) {
            std::vector<EdgePtr> v(edges.begin(), edges.end());
            std::sort(v.begin(), v.end(), [&](const EdgePtr& a, const EdgePtr& b) {
                return edge_id(a, view_) < edge_id(b, view_);
            });
            return v;
        };
        auto dumpNodeSet = [&](const char* label, const std::set<NodePtr>& nodes) {
            out << label << " (" << nodes.size() << ")\n";
            for (const auto& n : nodes) {
                out << "  - " << node_id(n) << "\n";
            }
        };

        out << "=== Incremental Region Analysis ===\n";
        out << "Delta nodes: " << last_delta_nodes_.size() << "\n";
        out << "Delta edges: " << last_delta_edges_.size() << "\n";
        out << "Region nodes: " << s.region_nodes << " / DR nodes: " << s.dr_nodes << "\n";
        out << "Region edges: " << s.region_edges << " / DR edges: " << s.dr_edges << "\n";
        out << "Recompute (optimized): " << s.optimized_recomputed
            << " ; (naive): " << s.naive_recomputed
            << " ; ratio: " << std::fixed << std::setprecision(3) << s.ratio()
            << " ; diff: " << s.diff() << "\n";
        out << "\n";

        if (have_initial_snapshot_) {
            out << "Initial region snapshot\n";
            out << "  nodes=" << initial_region_.nodes.size()
                << " edges=" << initial_region_.edges.size() << "\n";
            out << "  boundaries(out=" << initial_boundaries_.out_induced.size()
                << ", scope=" << initial_boundaries_.scope_induced.size()
                << ", residual=" << initial_boundaries_.residual.size() << ")\n";
            out << "  Region nodes (" << initial_region_.nodes.size() << ")\n";
            for (const auto& n : sortNodes(initial_region_.nodes)) {
                out << "    - " << node_id(n) << "\n";
            }
            out << "  Region edges (" << initial_region_.edges.size() << ")\n";
            for (const auto& e : sortEdges(initial_region_.edges)) {
                out << "    - " << edge_id(e, view_) << "\n";
            }
            out << "  Boundaries out_induced (" << initial_boundaries_.out_induced.size() << ")\n";
            for (const auto& n : initial_boundaries_.out_induced) {
                out << "    - " << node_id(n) << "\n";
            }
            out << "  Boundaries scope_induced (" << initial_boundaries_.scope_induced.size() << ")\n";
            for (const auto& n : initial_boundaries_.scope_induced) {
                out << "    - " << node_id(n) << "\n";
            }
            out << "  Boundaries residual (" << initial_boundaries_.residual.size() << ")\n";
            for (const auto& n : initial_boundaries_.residual) {
                out << "    - " << node_id(n) << "\n";
            }
            out << "\n";
        }

        if (!expandSnapshots_.empty()) {
            out << "Expand iterations (" << expandSnapshots_.size() << ")\n";
            for (const auto& snap : expandSnapshots_) {
                out << "  [" << snap.phase << " iter " << snap.iter << "]"
                    << " ms=" << std::fixed << std::setprecision(3) << snap.iterMs
                    << " boundary_before=" << snap.boundaryBefore
                    << " boundary_after=" << snap.boundaryAfter
                    << " blocking=" << snap.blockingCount
                    << " extended=" << (snap.extended ? 1 : 0)
                    << " reason=" << snap.reason
                    << " nodes=" << snap.region.nodes.size()
                    << " edges=" << snap.region.edges.size()
                    << "\n";

                out << "    Region nodes (" << snap.region.nodes.size() << ")\n";
                for (const auto& n : sortNodes(snap.region.nodes)) {
                    out << "      - " << node_id(n) << "\n";
                }
                out << "    Region edges (" << snap.region.edges.size() << ")\n";
                for (const auto& e : sortEdges(snap.region.edges)) {
                    out << "      - " << edge_id(e, view_) << "\n";
                }
                out << "    Boundaries out_induced (" << snap.boundaries.out_induced.size() << ")\n";
                for (const auto& n : snap.boundaries.out_induced) {
                    out << "      - " << node_id(n) << "\n";
                }
                out << "    Boundaries scope_induced (" << snap.boundaries.scope_induced.size() << ")\n";
                for (const auto& n : snap.boundaries.scope_induced) {
                    out << "      - " << node_id(n) << "\n";
                }
                out << "    Boundaries residual (" << snap.boundaries.residual.size() << ")\n";
                for (const auto& n : snap.boundaries.residual) {
                    out << "      - " << node_id(n) << "\n";
                }
            }
            out << "\n";
        }

        out << "Delta input nodes (" << last_delta_inputs_.size() << ")\n";
        for (const auto& n : sortNodes(last_delta_inputs_)) {
            out << "  - " << node_id(n) << "\n";
        }
        out << "Delta insert nodes (" << last_delta_nodes_.size() << ")\n";
        for (const auto& n : sortNodes(last_delta_nodes_)) {
            out << "  - " << node_id(n) << "\n";
        }
        out << "Delta insert edges (" << last_delta_edges_.size() << ")\n";
        for (const auto& e : sortEdges(last_delta_edges_)) {
            out << "  - " << edge_id(e, view_) << "\n";
        }
        out << "\n";

        out << "Region nodes (" << R.nodes.size() << ")\n";
        for (const auto& n : sortNodes(R.nodes)) {
            out << "  - " << node_id(n) << "\n";
        }
        out << "Region edges (" << R.edges.size() << ")\n";
        for (const auto& e : sortEdges(R.edges)) {
            out << "  - " << edge_id(e, view_) << "\n";
        }
        out << "\n";

        out << "Delta-reachable nodes (" << dr_.nodes.size() << ")\n";
        for (const auto& n : sortNodes(dr_.nodes)) {
            out << "  - " << node_id(n) << "\n";
        }
        out << "Delta-reachable edges (" << dr_.edges.size() << ")\n";
        for (const auto& e : sortEdges(dr_.edges)) {
            out << "  - " << edge_id(e, view_) << "\n";
        }
        out << "\n";

        dumpNodeSet("Boundary out_induced", B.out_induced);
        dumpNodeSet("Boundary scope_induced", B.scope_induced);
        dumpNodeSet("Boundary residual", B.residual);
        out << "\n";

        out << "Mergeable anchors by head (" << last_analysis_.mergeableAnchorsByHead.size() << ")\n";
        std::vector<NodePtr> heads;
        heads.reserve(last_analysis_.mergeableAnchorsByHead.size());
        for (const auto& kv : last_analysis_.mergeableAnchorsByHead) {
            heads.push_back(kv.first);
        }
        std::sort(heads.begin(), heads.end(),
                [](const NodePtr& a, const NodePtr& b) { return node_id(a) < node_id(b); });
        for (const auto& head : heads) {
            const auto& anchors = last_analysis_.mergeableAnchorsByHead.at(head);
            out << "  head " << node_id(head) << " (" << anchors.size() << " anchors)\n";
            for (const auto& anchor : anchors) {
                out << "    - " << anchor_id(anchor, view_) << "\n";
            }
        }
    }

    void emitLegend_(std::ofstream& out, bool includeMerge) const {
        out << "  subgraph cluster_legend {\n";
        out << "    label=\"Legend\";\n";
        out << "    color=gray;\n";
        out << "    legend_fact [shape=box, label=\"Fact node\"];\n";
        out << "    legend_derived [shape=ellipse, label=\"Derived node\"];\n";
        out << "    legend_region [shape=ellipse, penwidth=2, color=\"#1f77b4\", label=\"Region member\"];\n";
        out << "    legend_boundary [shape=ellipse, penwidth=2, color=\"#ff7f0e\", label=\"Boundary head\"];\n";
        out << "    legend_delta_node [shape=ellipse, penwidth=2, color=\"#2ca02c\", label=\"Delta node\"];\n";
        out << "    legend_anchor_node [shape=box, penwidth=2, color=\"#f2c744\", label=\"Mergeable node anchor\"];\n";
        out << "    legend_src_region [shape=box, label=\"Sample fact\"];\n";
        out << "    legend_dst_region [shape=ellipse, label=\"Sample head\"];\n";
        out << "    legend_hyper_region [shape=point, width=0.2, height=0.2, label=\"\", style=filled, color=\"#1f77b4\", fillcolor=\"#1f77b4\"];\n";
        out << "    legend_src_region -> legend_hyper_region [color=\"#1f77b4\", penwidth=2, label=\"region hyperedge\"];\n";
        out << "    legend_hyper_region -> legend_dst_region [color=\"#1f77b4\", penwidth=2];\n";
        out << "    legend_src_delta [shape=box, label=\"Sample fact\"];\n";
        out << "    legend_dst_delta [shape=ellipse, label=\"Sample head\"];\n";
        out << "    legend_hyper_delta [shape=point, width=0.2, height=0.2, label=\"\", style=filled, color=\"#2ca02c\", fillcolor=\"#2ca02c\"];\n";
        out << "    legend_src_delta -> legend_hyper_delta [color=\"#2ca02c\", penwidth=2, label=\"delta hyperedge\"];\n";
        out << "    legend_hyper_delta -> legend_dst_delta [color=\"#2ca02c\", penwidth=2];\n";
        if (includeMerge) {
            out << "    legend_src_merge [shape=box, label=\"Sample fact\"];\n";
            out << "    legend_dst_merge [shape=ellipse, label=\"Sample head\"];\n";
            out << "    legend_hyper_merge [shape=point, width=0.2, height=0.2, label=\"\", style=filled, color=\"red\", fillcolor=\"red\"];\n";
            out << "    legend_src_merge -> legend_hyper_merge [color=\"red\", penwidth=2, label=\"mergeable anchor path\"];\n";
            out << "    legend_hyper_merge -> legend_dst_merge [color=\"red\", penwidth=2];\n";
        }
        out << "  }\n";
    }

    static std::string scopeStr_(const std::set<NodePtr>& ss) {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (auto& n : ss) {
            if (!first) oss << "; ";
            first = false;
            oss << node_id(n);
        }
        oss << "}";
        return oss.str();
    }

    static std::string quote_(const std::string& s) {
        std::ostringstream oss;
        oss << "\"";
        for (char c : s) {
            if (c=='"') oss << "\\\"";
            else if (c=='\\') oss << "\\\\";
            else oss << c;
        }
        oss << "\"";
        return oss.str();
    }

private:
    DGView& view_;

    // Derived artifacts
    std::unordered_map<NodePtr, std::set<NodePtr>> lp_set_;     // least-parents set
    std::unordered_map<NodePtr, std::set<NodePtr>> node_scope_; // node -> scope (base facts)

    // Scope caches keyed by source node
    std::unordered_map<NodePtr, std::set<EdgePtr>> edge_scope_by_source_;
    std::unordered_map<EdgePtr, std::set<NodePtr>> edge_scope_index_;
    std::unordered_map<NodePtr, std::set<NodePtr>> reachable_cache_;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> full_reachable_cache_;
    std::unordered_map<size_t, std::unordered_set<size_t>> scc_reach_cache_;
    std::unordered_map<size_t, std::set<NodePtr>> scc_reach_nodes_cache_;
    std::unordered_map<NodePtr, std::vector<NodePtr>> head_branching_sources_cache_;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> head_scope_owners_cache_;
    std::unordered_set<NodePtr> head_scope_index_ready_;
    std::unordered_map<NodePtr, std::vector<IncRegionAnalysis::AnchorCandidate>> anchor_candidates_cache_;
    bool scope_index_global_ready_ = false;

    // Structural helpers
    std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming_edges_map_;
    std::unordered_map<NodePtr, std::vector<EdgePtr>> outgoing_edges_map_;
    std::unordered_map<NodePtr, std::set<NodePtr>> preds_;
    std::unordered_map<NodePtr, std::set<NodePtr>> succs_;
    std::unordered_set<EdgePtr> delta_insert_edges_cache_;
    std::unordered_set<NodePtr> delta_insert_nodes_cache_;
    bool delta_out_nodes_ready_ = false;
    bool delta_lp_union_ready_ = false;
    std::unordered_set<NodePtr> delta_out_nodes_cache_;
    std::unordered_set<NodePtr> delta_lp_union_cache_;

    struct ExpandSnapshot {
        std::string phase;
        int iter = 0;
        double iterMs = 0.0;
        size_t boundaryBefore = 0;
        size_t boundaryAfter = 0;
        size_t blockingCount = 0;
        bool extended = false;
        std::string reason;
        Region region;
        Boundaries boundaries;
    };
    std::vector<ExpandSnapshot> expandSnapshots_;
    bool have_initial_snapshot_ = false;
    Region initial_region_;
    Boundaries initial_boundaries_;

    // Last run
    Region     last_region_;
    Boundaries last_boundaries_;
    Region     dr_;
    IncRegionAnalysis last_analysis_;
    bool       have_last_ = false;
    size_t     last_scope_nodes_total_ = 0;
    size_t     last_scope_nodes_extra_ = 0;
    size_t     last_scope_edges_total_ = 0;
    size_t     last_scope_edges_extra_ = 0;
    std::set<NodePtr> last_delta_nodes_;
    std::set<EdgePtr> last_delta_edges_;
    std::set<NodePtr> last_delta_inputs_;
    std::set<NodePtr> current_delta_sources_;
    ReachInfo reach_filter_;

    const std::unordered_set<size_t>& getReachSccs_(size_t cid) {
        auto it = scc_reach_cache_.find(cid);
        if (it != scc_reach_cache_.end()) return it->second;
        auto& scc = view_.getCycleDependencyGraph();
        std::unordered_set<size_t> reach{cid};
        if (cid < scc.reverseDependencies.size()) {
            for (auto succ : scc.reverseDependencies[cid]) {
                const auto& succReach = getReachSccs_(succ);
                reach.insert(succReach.begin(), succReach.end());
            }
        }
        return scc_reach_cache_.emplace(cid, std::move(reach)).first->second;
    }

    const std::set<NodePtr>& getReachNodes_(size_t cid) {
        auto it = scc_reach_nodes_cache_.find(cid);
        if (it != scc_reach_nodes_cache_.end()) return it->second;
        auto& scc = view_.getCycleDependencyGraph();
        std::set<NodePtr> nodes;
        const auto& reachSccs = getReachSccs_(cid);
        for (auto rid : reachSccs) {
            if (rid >= scc.nodeCycles.size()) continue;
            const auto& group = scc.nodeCycles[rid];
            nodes.insert(group.begin(), group.end());
        }
        return scc_reach_nodes_cache_.emplace(cid, std::move(nodes)).first->second;
    }

    const std::set<NodePtr>& ensureReachable_(const NodePtr& source) {
        auto it = reachable_cache_.find(source);
        if (it != reachable_cache_.end()) return it->second;
        // In lazy mode, avoid forcing SCC construction; a plain DFS is typically cheaper for a small
        // number of sources.
        return reachable_cache_.emplace(source, forwardReachable_(source)).first->second;
    }

    const std::unordered_set<NodePtr>& ensureFullReachable_(const NodePtr& source) {
        static const std::unordered_set<NodePtr> kEmpty;
        if (!source) return kEmpty;
        auto it = full_reachable_cache_.find(source);
        if (it != full_reachable_cache_.end()) return it->second;
        std::unordered_set<NodePtr> reachable;
        std::vector<NodePtr> stack;
        stack.push_back(source);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (!cur || !reachable.insert(cur).second) continue;
            for (const auto& edge : view_.getOutgoingEdges(cur)) {
                if (!edge) continue;
                NodePtr head = view_.getOutput(edge);
                if (head) stack.push_back(head);
            }
        }
        return full_reachable_cache_.emplace(source, std::move(reachable)).first->second;
    }

    void ensureLeastParents_(const NodePtr& source) {
        if (!source) return;
        if (lp_set_.find(source) != lp_set_.end()) return;
        if (!view_.getValidNodes().count(source)) {
            lp_set_[source] = {};
            return;
        }
        auto outIt = outgoing_edges_map_.find(source);
        if (outIt == outgoing_edges_map_.end() || outIt->second.size() < 2) {
            lp_set_[source] = {};
            return;
        }
        const auto& reachable = ensureReachable_(source);
        std::unordered_map<NodePtr, size_t> branch_counts;
        for (auto& edge : outIt->second) {
            if (!edge) continue;
            auto child = view_.getOutput(edge);
            if (!child) continue;
            auto branchReach = forwardReachableFromBranch_(child, source);
            for (auto& node : branchReach) {
                if (node.get() == source.get()) continue;
                branch_counts[node]++;
            }
        }
        std::set<NodePtr> merge_nodes;
        for (auto& [node, count] : branch_counts) {
            if (count < 2 || !reachable.count(node)) continue;
            auto inIt = incoming_edges_map_.find(node);
            if (inIt == incoming_edges_map_.end()) continue;
            if (inIt->second.size() < 2) continue;
            merge_nodes.insert(node);
        }
        if (merge_nodes.empty()) {
            lp_set_[source] = {};
            return;
        }
        auto dom = computeDominators_(source, reachable);
        std::set<NodePtr> least;
        for (auto& m : merge_nodes) {
            bool dominated = false;
            for (auto& other : merge_nodes) {
                if (m.get() == other.get()) continue;
                auto dit = dom.find(m);
                if (dit != dom.end() && dit->second.count(other)) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) least.insert(m);
        }
        lp_set_[source] = std::move(least);
    }

    void ensureScope_(const NodePtr& source) {
        if (!source) return;
        if (node_scope_.find(source) != node_scope_.end()) return;
        if (!view_.getValidNodes().count(source)) {
            node_scope_[source] = {};
            edge_scope_by_source_[source] = {};
            return;
        }
        ensureLeastParents_(source);
        std::set<NodePtr> scope_nodes;
        std::set<EdgePtr> scope_edges;
        auto lp_it = lp_set_.find(source);
        auto reach_it = reachable_cache_.find(source);
        if (lp_it != lp_set_.end() && reach_it != reachable_cache_.end()) {
            for (auto& m : lp_it->second) {
                collectScopeFrom_(m, reach_it->second, scope_nodes, scope_edges);
            }
        }
        node_scope_[source] = scope_nodes;
        edge_scope_by_source_[source] = scope_edges;
        for (auto& e : scope_edges) {
            edge_scope_index_[e].insert(source);
        }
    }

    const std::vector<NodePtr>& getBranchingSourcesForHead_(const NodePtr& head) {
        static const std::vector<NodePtr> kEmpty;
        if (!head) return kEmpty;
        auto it = head_branching_sources_cache_.find(head);
        if (it != head_branching_sources_cache_.end()) return it->second;
        const auto& validNodes = view_.getValidNodes();
        std::unordered_set<NodePtr> visited;
        std::vector<NodePtr> stack;
        stack.push_back(head);
        visited.insert(head);
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            const auto& preds = getPreds_(cur);
            if (preds.empty()) continue;
            for (auto& pred : preds) {
                if (visited.insert(pred).second) {
                    stack.push_back(pred);
                }
            }
        }
        std::vector<NodePtr> sources;
        sources.reserve(visited.size());
        for (auto& n : visited) {
            if (!validNodes.count(n)) continue;
            auto outIt = outgoing_edges_map_.find(n);
            if (outIt != outgoing_edges_map_.end() && outIt->second.size() >= 2) {
                sources.push_back(n);
            }
        }
        return head_branching_sources_cache_.emplace(head, std::move(sources)).first->second;
    }

    void ensureHeadScopeIndex_(const NodePtr& head) {
        if (!head) return;
        if (scope_index_global_ready_) return;
        if (head_scope_index_ready_.count(head)) return;
        const auto& sources = getBranchingSourcesForHead_(head);
        for (const auto& s : sources) {
            ensureScope_(s);
        }
        head_scope_index_ready_.insert(head);
    }

    void ensureDeltaOutNodes_() {
        if (delta_out_nodes_ready_) return;
        for (auto& e : last_delta_edges_) {
            delta_out_nodes_cache_.insert(view_.getOutput(e));
        }
        delta_out_nodes_ready_ = true;
    }

    void ensureDeltaLpUnion_() {
        if (delta_lp_union_ready_) return;
        for (auto& x : last_delta_nodes_) {
            if (!reach_filter_.nodes.empty() && !reach_filter_.nodes.count(x)) {
                continue;
            }
            auto oit = outgoing_edges_map_.find(x);
            if (oit == outgoing_edges_map_.end() || oit->second.size() < 2) {
                continue;
            }
            ensureLeastParents_(x);
            auto it = lp_set_.find(x);
            if (it != lp_set_.end()) {
                delta_lp_union_cache_.insert(it->second.begin(), it->second.end());
            }
        }
        delta_lp_union_ready_ = true;
    }

    void debugPrintLeastParents_() {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) return;
        std::cout << "[lp] least parents per node:\n";
        for (auto& [node, parents] : lp_set_) {
            std::cout << "  " << node_id(node) << " <- {";
            bool first = true;
            for (auto& p : parents) {
                if (!first) std::cout << ", ";
                std::cout << node_id(p);
                first = false;
            }
            std::cout << "}\n";
        }
    }

    void debugPrintNodeScopes_() {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) return;
        std::cout << "[scope] node scopes:\n";
        for (auto& [node, scope] : node_scope_) {
            std::cout << "  " << node_id(node) << " scope=" << scopeStr_(scope) << "\n";
        }
    }

    void debugPrintEdgeScopes_() {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) return;
        std::cout << "[scope] edge scopes:\n";
        for (auto& e : view_.getValidEdges()) {
            std::cout << "  " << edge_id(e, view_) << " scope=" << scopeStr_(edgeScope_(e)) << "\n";
        }
    }
};

} // namespace incra
