#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension.hpp"

namespace duckdb {

class DryrunExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
