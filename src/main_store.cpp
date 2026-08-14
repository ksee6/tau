/**
 * @file main_store.cpp
 * @brief tau-store entry point.
 *
 * This binary is installed SUID root (chown root:root && chmod u+s).
 * It reads all manifests in TAU_PATH/manifests/, runs unrun ones inside
 * isolated namespaces, then garbage-collects unreferenced store entries.
 *
 * Usage: tau-store
 *   (no arguments — it acts on the full manifests/ directory)
 */

#include <Tau/Store.hpp>
#include <Tau/Config.hpp>
#include <Terminal/Format.hpp>
#include <Xi/Log.hpp>

int main(int /*argc*/, char ** /*argv*/) {
    Tau::Config::init();
    Tau::Config::ensureLayout();

    return Tau::Store::run();
}
