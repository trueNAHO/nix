#include "command.hh"
#include "common-args.hh"
#include "shared.hh"
#include "store-api.hh"
#include "local-fs-store.hh"
#include "progress-bar.hh"

#include <nlohmann/json.hpp>

using namespace nix;

static nlohmann::json derivedPathsToJSON(const DerivedPaths & paths, Store & store)
{
    auto res = nlohmann::json::array();
    for (auto & t : paths) {
        std::visit([&](const auto & t) {
            res.push_back(t.toJSON(store));
        }, t.raw());
    }
    return res;
}

static nlohmann::json builtPathsWithResultToJSON(const std::vector<BuiltPathWithResult> & buildables, const Store & store)
{
    auto res = nlohmann::json::array();
    for (auto & b : buildables) {
        std::visit([&](const auto & t) {
            auto j = t.toJSON(store);
            if (b.result) {
                if (b.result->startTime)
                    j["startTime"] = b.result->startTime;
                if (b.result->stopTime)
                    j["stopTime"] = b.result->stopTime;
                if (b.result->cpuUser)
                    j["cpuUser"] = ((double) b.result->cpuUser->count()) / 1000000;
                if (b.result->cpuSystem)
                    j["cpuSystem"] = ((double) b.result->cpuSystem->count()) / 1000000;
            }
            res.push_back(j);
        }, b.path.raw());
    }
    return res;
}

// TODO deduplicate with other code also setting such out links.
static void createOutLinks(
    const std::filesystem::path & outLink,
    const std::vector<BuiltPathWithResult> & buildables,
    LocalFSStore & store2,
    PathSet & symlinks)
{
    for (const auto & [_i, buildable] : enumerate(buildables)) {
        auto i = _i;
        std::visit(overloaded {
            [&](const BuiltPath::Opaque & bo) {
                auto symlink = absPath(outLink.string());
                if (i) symlink += fmt("-%d", i);
                store2.addPermRoot(bo.path, symlink);
                symlinks.insert(symlink);
            },
            [&](const BuiltPath::Built & bfd) {
                for (auto & output : bfd.outputs) {
                    auto symlink = absPath(outLink.string());
                    if (i) symlink += fmt("-%d", i);
                    if (output.first != "out") symlink += fmt("-%s", output.first);
                    store2.addPermRoot(output.second, symlink);
                    symlinks.insert(symlink);
                }
            },
        }, buildable.path.raw());
    }
}

struct CmdBuild : InstallablesCommand, MixDryRun, MixJSON, MixProfile
{
    Path outLink = "result";
    bool printOutputPaths = false;
    BuildMode buildMode = bmNormal;

    CmdBuild()
    {
        addFlag({
            .longName = "out-link",
            .shortName = 'o',
            .description = "Use *path* as prefix for the symlinks to the build results. It defaults to `result`.",
            .labels = {"path"},
            .handler = {&outLink},
            .completer = completePath
        });

        addFlag({
            .longName = "no-link",
            .description = "Do not create symlinks to the build results.",
            .handler = {&outLink, Path("")},
        });

        addFlag({
            .longName = "print-out-paths",
            .description = "Print the resulting output paths",
            .handler = {&printOutputPaths, true},
        });

        addFlag({
            .longName = "rebuild",
            .description = "Rebuild an already built package and compare the result to the existing store paths.",
            .handler = {&buildMode, bmCheck},
        });
    }

    std::string description() override
    {
        return "build a derivation or fetch a store path";
    }

    std::string doc() override
    {
        return
          #include "build.md"
          ;
    }

    void run(ref<Store> store, Installables && installables) override
    {
        if (dryRun) {
            std::vector<DerivedPath> pathsToBuild;

            for (auto & i : installables)
                for (auto & b : i->toDerivedPaths())
                    pathsToBuild.push_back(b.path);

            printMissing(store, pathsToBuild, lvlError);

            if (json)
                logger->cout("%s", derivedPathsToJSON(pathsToBuild, *store).dump());

            return;
        }

        auto buildables = Installable::build(
            getEvalStore(), store,
            Realise::Outputs,
            installables,
            repair ? bmRepair : buildMode);

        if (json) logger->cout("%s", builtPathsWithResultToJSON(buildables, *store).dump());

        PathSet symlinks;

        if (outLink != "")
            if (auto store2 = store.dynamic_pointer_cast<LocalFSStore>())
                createOutLinks(outLink, buildables, *store2, symlinks);

        if (printOutputPaths) {
            stopProgressBar();
            for (auto & buildable : buildables) {
                std::visit(overloaded {
                    [&](const BuiltPath::Opaque & bo) {
                        logger->cout(store->printStorePath(bo.path));
                    },
                    [&](const BuiltPath::Built & bfd) {
                        for (auto & output : bfd.outputs) {
                            logger->cout(store->printStorePath(output.second));
                        }
                    },
                }, buildable.path.raw());
            }
        }

        BuiltPaths buildables2;
        for (auto & b : buildables)
            buildables2.push_back(b.path);
        updateProfile(buildables2);

        if (!json)
            notice(
                ANSI_GREEN "Build succeeded." ANSI_NORMAL
                " The result is available through the symlink " ANSI_BOLD "%s" ANSI_NORMAL ".",
                showPaths(symlinks));
    }
};

static auto rCmdBuild = registerCommand<CmdBuild>("build");
