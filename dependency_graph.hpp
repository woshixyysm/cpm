#pragma once
#include "module_unit.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <functional>

namespace cppm {

struct GraphNode {
    ModuleUnit unit;
    std::vector<std::string> dependsOn; // node_keys this unit requires
};

class CycleError : public std::runtime_error {
public:
    explicit CycleError(const std::string& cyclePath)
        : std::runtime_error("Module dependency cycle detected: " + cyclePath),
          cyclePath_(cyclePath) {}
    const std::string& cyclePath() const { return cyclePath_; }
private:
    std::string cyclePath_;
};

class UnresolvedDependencyError : public std::runtime_error {
public:
    UnresolvedDependencyError(const std::string& dep, const std::string& requiredBy)
        : std::runtime_error("Unresolved module dependency '" + dep +
                              "' required by '" + requiredBy + "'"),
          dependency_(dep), requiredBy_(requiredBy) {}
    const std::string& dependency() const { return dependency_; }
    const std::string& requiredBy() const { return requiredBy_; }
private:
    std::string dependency_, requiredBy_;
};

struct VersionConflict {
    std::string moduleName;
    std::string packageA, hashA;
    std::string packageB, hashB;
};

// A DAG over module compilation units, keyed by node_key(unit)
// ("scope.module" or "scope.module:partition"). Supports:
//   - ingestion from multiple packages with conflict detection
//   - cycle detection with a human-readable cycle path
//   - wave-based topological sort (parallel-build friendly)
class DependencyGraph {
public:
    // Adds a unit to the graph. If a differently-sourced unit already
    // occupies the same node key with a different content hash, records
    // a VersionConflict (does not throw; caller decides how to handle it).
    void addUnit(const ModuleUnit& unit) {
        const std::string key = node_key(unit);

        if (auto it = nodes_.find(key); it != nodes_.end()) {
            const auto& existing = it->second.unit;
            if (existing.owningPackage != unit.owningPackage &&
                existing.contentHash != unit.contentHash) {
                conflicts_.push_back(VersionConflict{
                    key, existing.owningPackage, existing.contentHash,
                    unit.owningPackage, unit.contentHash
                });
            }
        }

        GraphNode node;
        node.unit = unit;
        for (const auto& dep : unit.imports) {
            if (dep.isHeaderUnit) continue; // header units are not graph edges
            node.dependsOn.push_back(dep.name);
        }
        nodes_[key] = std::move(node);
    }

    bool hasNode(const std::string& key) const { return nodes_.contains(key); }
    size_t size() const { return nodes_.size(); }

    const std::vector<VersionConflict>& conflicts() const { return conflicts_; }

    // Marks a node key as externally pre-resolved (e.g. `std`, `std.compat`,
    // or a header unit alias) so dependency resolution doesn't fail on it
    // even though no ModuleUnit was scanned for it locally.
    void registerExternalNode(const std::string& key, std::string owningPackage = "<external>") {
        if (nodes_.contains(key)) return;
        GraphNode node;
        node.unit.logicalName = key;
        node.unit.kind = UnitKind::PrimaryInterface;
        node.unit.owningPackage = std::move(owningPackage);
        nodes_[key] = std::move(node);
    }

    // Returns build "waves": each inner vector's modules may be compiled in
    // parallel; wave[i] depends only on modules present in wave[0..i-1].
    // Throws UnresolvedDependencyError or CycleError on failure.
    std::vector<std::vector<std::string>> topologicalWaves() const {
        std::unordered_map<std::string, int> indegree;
        std::unordered_map<std::string, std::vector<std::string>> adjacency; // dep -> dependents

        for (const auto& [key, node] : nodes_) {
            indegree.try_emplace(key, 0);
        }
        for (const auto& [key, node] : nodes_) {
            for (const auto& dep : node.dependsOn) {
                if (!nodes_.contains(dep)) {
                    throw UnresolvedDependencyError(dep, key);
                }
                adjacency[dep].push_back(key);
                indegree[key]++;
            }
        }

        std::deque<std::string> ready;
        for (auto& [key, deg] : indegree) {
            if (deg == 0) ready.push_back(key);
        }

        std::vector<std::vector<std::string>> waves;
        size_t visited = 0;

        while (!ready.empty()) {
            std::vector<std::string> wave(ready.begin(), ready.end());
            std::sort(wave.begin(), wave.end()); // deterministic output
            waves.push_back(wave);
            ready.clear();

            for (const auto& key : wave) {
                ++visited;
                auto it = adjacency.find(key);
                if (it == adjacency.end()) continue;
                for (const auto& dependent : it->second) {
                    if (--indegree[dependent] == 0) ready.push_back(dependent);
                }
            }
        }

        if (visited != nodes_.size()) {
            throw CycleError(describeCycle(indegree));
        }
        return waves;
    }

    // Flat topological order (single sequence), convenience wrapper over waves.
    std::vector<std::string> topologicalOrder() const {
        std::vector<std::string> flat;
        for (auto& wave : topologicalWaves())
            for (auto& k : wave) flat.push_back(k);
        return flat;
    }

    const std::unordered_map<std::string, GraphNode>& nodes() const { return nodes_; }

    // All partitions declared under a given primary module's logical name.
    std::vector<std::string> partitionsOf(const std::string& logicalModuleName) const {
        std::vector<std::string> result;
        for (const auto& [key, node] : nodes_) {
            if (node.unit.isPartitionUnit() && node.unit.logicalName == logicalModuleName) {
                result.push_back(key);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    std::unordered_map<std::string, GraphNode> nodes_;
    std::vector<VersionConflict> conflicts_;

    // Best-effort DFS-based cycle path reconstruction, used only for diagnostics
    // once we already know (via Kahn's algorithm leftovers) that a cycle exists.
    std::string describeCycle(const std::unordered_map<std::string, int>& remainingIndegree) const {
        std::unordered_set<std::string> inCycle;
        for (const auto& [k, deg] : remainingIndegree) {
            if (deg > 0) inCycle.insert(k);
        }

        std::unordered_map<std::string, int> color; // 0=white,1=gray,2=black
        std::vector<std::string> stack;
        std::string found;

        std::function<bool(const std::string&)> dfs = [&](const std::string& u) -> bool {
            color[u] = 1;
            stack.push_back(u);
            auto it = nodes_.find(u);
            if (it != nodes_.end()) {
                for (const auto& v : it->second.dependsOn) {
                    if (!inCycle.contains(v)) continue;
                    if (color[v] == 1) {
                        std::string path;
                        auto pos = std::find(stack.begin(), stack.end(), v);
                        for (auto p = pos; p != stack.end(); ++p) { path += *p; path += " -> "; }
                        path += v;
                        found = path;
                        return true;
                    }
                    if (color[v] == 0 && dfs(v)) return true;
                }
            }
            stack.pop_back();
            color[u] = 2;
            return false;
        };

        for (const auto& k : inCycle) {
            if (color[k] == 0 && dfs(k)) break;
        }
        return found.empty() ? "<unknown cycle>" : found;
    }
};

} // namespace cppm