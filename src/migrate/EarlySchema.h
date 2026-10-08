#pragma once

#include <filesystem>

namespace mira::migrate {

// mira.db and cache.db made by unreleased builds between the move to SQLite and folder sorting
// by tag: their schema step 1 lacks what step 1 has since (games.library_link and
// games.folder_tag, cache.db's lists). Adds what's missing, before the stores open them.
//
// Temporary. To drop it: delete src/migrate/EarlySchema.*, its call in mirad_main.cpp and
// tests/early_schema_test.cpp.
void RepairEarlySchemas(const std::filesystem::path& library_db,
                        const std::filesystem::path& cache_db);

}  // namespace mira::migrate
