// module_unit.hpp
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace cppm {

enum class UnitKind {
    PrimaryInterface,   // export module M;
    Partition,          // export module M:part;
    Implementation,     // module M;  (implicit import of primary)
    ImplPartition,      // module M:part; (non-exported partition impl)
    Unknown
};

struct ModuleDependency {
    std::string name;        // "fmt.core", "std", or partition-local ":impl_detail"
    bool isPartition = false;
    bool isExportImport = false; // `export import X;` — re-exported dependency
    bool isHeaderUnit = false;   // true if this dependency is an imported header unit
};

struct ModuleUnit {
    UnitKind kind = UnitKind::Unknown;
    std::string logicalName;          // "scope.module" (empty for pure impl. units w/o own name)
    std::string partitionName;        // set if kind is Partition/ImplPartition
    std::filesystem::path filePath;
    std::vector<ModuleDependency> imports;
    std::string contentHash;          // filled by scanner (sha256 of normalized source)
    std::string owningPackage;        // "scope/name@version" for conflict tracking

    bool isPartitionUnit() const {
        return kind == UnitKind::Partition || kind == UnitKind::ImplPartition;
    }
};

// The graph identifies nodes by a fully-qualified key:
//   "scope.module"            for primary interfaces
//   "scope.module:partition"  for partitions
inline std::string node_key(const ModuleUnit& u) {
    if (u.kind == UnitKind::Partition || u.kind == UnitKind::ImplPartition) {
        // Partitions belong to their owning module's logical name, carried separately
        return u.logicalName + ":" + u.partitionName;
    }
    return u.logicalName;
}

} // namespace cppm