/**
 * @file main_instance.cpp
 * @brief tau-instance entry point.
 */

#include <Tau/Spawn.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>
#include <Terminal/Command.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    using namespace Terminal;
    using namespace Tau;

    Command args(argc, argv);
    args.description("tau-instance — internal instance supervisor");

    String name         = args.option("--name -n").string();
    String manifestOpt  = args.option("--manifest -m").string();
    String srcManifest  = args.option("--source-manifest").string();
    String workDir      = args.option("--workdir -w").string();
    bool   headless     = args.flag("--headless -h");
    bool   remove_      = args.flag("--remove -r");
    bool   global_      = args.flag("--global -g");
    bool   detach       = args.flag("--detach -d");
    bool   attachStdin  = args.flag("--attach-stdin");
    String detachKey    = args.option("--detach-key -k").defaults("ctrl+b").string();
    String colOpt       = args.option("--col -c").defaults("auto").string();
    String rowOpt       = args.option("--row -r").defaults("auto").string();
    bool   debug        = args.flag("--debug -v");
    if (debug) g_debugMode = true;

    Array<String> entries;
    for (size_t i = 0; ; ++i) {
        String p = args[i];
        if (p.isEmpty()) break;
        if (p == "--") break;
        entries.push(p);
    }
    if (entries.length() == 0 && !manifestOpt.isEmpty()) {
        entries.push(manifestOpt);
    }

    if (entries.length() == 0) {
        logError("tau-instance: at least one manifest entry is required");
        return 1;
    }

    Config::init();
    Config::ensureLayout();

    if (name.isEmpty()) {
        // Try reading name from the last entry
        Array<String> visited;
        ParseError perr;
        ParsedManifest pm = Manifest::load(entries[entries.length() - 1], visited, perr);
        if (perr.ok && !pm.meta.name.isEmpty()) {
            name = pm.meta.name;
        } else {
            name = "0";
        }
    }

    if (!workDir.isEmpty()) {
        (void)::chdir(workDir.c_str());
    }

    if (debug) {
        String logFile = Config::instanceDir(name) + "/spawn.log";
        int lfd = ::open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) {
            ::dup2(lfd, STDERR_FILENO);
            ::dup2(lfd, STDOUT_FILENO);
            ::close(lfd);
        }
    }

    SpawnOptions opts;
    opts.instanceName       = name;
    opts.instanceDir        = Config::instanceDir(name);
    opts.manifestPaths      = entries;
    opts.manifestPath       = entries[entries.length() - 1];
    opts.sourceManifestPath = srcManifest;
    opts.headless           = headless || remove_;
    opts.removeOnRead       = remove_;
    opts.globalMode         = global_;
    opts.attachStdin        = attachStdin;
    opts.detach             = detach;
    opts.detachKey          = detachKey;

    if (!colOpt.isEmpty() && colOpt != "auto") {
        opts.cols = (int)Collection::parseLong(colOpt);
    }
    if (!rowOpt.isEmpty() && rowOpt != "auto") {
        opts.rows = (int)Collection::parseLong(rowOpt);
    }

    SpawnProcess sp(opts);
    return sp.run();
}

