// skyrim_world.cpp - concrete ISkyrimWorld implementation + factory.
//
// The only translation unit that owns module lifetime. Everything else is
// reachable through the interfaces declared in the public headers.

#include "skyrim/skyrim_module.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "skyrim/form_database.hpp"
#include "skyrim/string_table.hpp"

namespace skyrim {
namespace {

void emit(const SkyrimConfig& cfg, const char* fmt, ...) {
    if (!cfg.logSink) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cfg.logSink(buf);
}

class SkyrimWorld final : public ISkyrimWorld {
public:
    Status init(const SkyrimConfig& config) override {
        if (m_up) shutdown();

        m_config = config;

        if (config.dataDirectory.empty()) {
            emit(config, "[skyrim] no dataDirectory configured");
            return Status::NotConfigured;
        }

        const Status st = m_db.load(config);
        if (st != Status::Ok) {
            emit(config, "[skyrim] form database load failed: %s", toString(st));
            return st;
        }

        emit(config, "[skyrim] loaded %zu forms from %zu source files "
                     "(master v%.2f, next object id 0x%06X)",
             m_db.formCount(), m_db.sourceCount(),
             static_cast<double>(m_db.masterHeader().version),
             m_db.masterHeader().nextObjectId);

        // Localized string tables (loose files; BSA-backed strings follow).
        // Best-effort: a missing or unreadable table must not fail init().
        m_strings.loadLoose(config.dataDirectory, "Skyrim", m_language);
        m_strings.loadLoose(config.dataDirectory, "Update", m_language);
        if (m_strings.tableCount() > 0) {
            emit(config, "[skyrim] loaded %zu string tables (%zu entries)",
                 m_strings.tableCount(), m_strings.totalEntries());
        } else {
            emit(config, "[skyrim] no localized string tables found "
                         "(expected for base-game strings, which ship in BSA)");
        }

        m_up = true;
        return Status::Ok;
    }

    void update(float deltaTime) override {
        if (!m_up || !(deltaTime > 0.0f)) return;
        m_time += deltaTime;
        // Simulation of the world subsystems (cells, actors, rpg, magic,
        // quests) is layered on here as those systems land; the asset/record
        // foundation is what init() establishes today.
    }

    void shutdown() override {
        if (!m_up) return;
        m_db.clear();
        m_time = 0.0f;
        m_up = false;
    }

    FormDatabase* database() override { return &m_db; }
    const FormDatabase* database() const override { return &m_db; }

    // Localized string resolution (FULL/DESC ids -> text). May be empty if the
    // install ships its strings inside BSA archives only.
    StringResolver& strings() override { return m_strings; }
    const StringResolver& strings() const override { return m_strings; }

    // Language for loose .STRINGS lookups, e.g. "English".
    void setLanguage(std::string lang) override { m_language = std::move(lang); }

private:
    SkyrimConfig  m_config;
    FormDatabase  m_db;
    StringResolver m_strings;
    std::string   m_language = "English";
    float         m_time = 0.0f;
    bool          m_up = false;
};

}  // namespace

ISkyrimWorld* createSkyrimWorld() { return new SkyrimWorld(); }

void destroySkyrimWorld(ISkyrimWorld* world) {
    if (!world) return;
    world->shutdown();
    delete world;
}

ModuleVersion moduleVersion() { return ModuleVersion{0, 1, 0}; }

}  // namespace skyrim
