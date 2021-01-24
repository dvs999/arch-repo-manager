#include "../librepomgr/serversetup.h"

#include "../libpkg/data/config.h"

#include "resources/config.h"

#include <c++utilities/application/argumentparser.h>
#include <c++utilities/io/ansiescapecodes.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace std;
using namespace CppUtilities;
using namespace CppUtilities::EscapeCodes;
using namespace LibRepoMgr;
using namespace LibPkg;

int main(int argc, const char *argv[])
{
    SET_APPLICATION_INFO;

    // define default server setup
    ServiceSetup setup;

    // read cli args
    ArgumentParser parser;
    OperationArgument runArg("run", 'r', "runs the server");
    ConfigValueArgument configFileArg("config-file", 'c', "specifies the path of the config file", { "path" });
    configFileArg.setEnvironmentVariable(PROJECT_VARNAME_UPPER "_CONFIG_FILE");
    runArg.setSubArguments({ &configFileArg });
    runArg.setImplicit(true);
    runArg.setCallback([&setup, &configFileArg](const ArgumentOccurrence &) {
        if (configFileArg.firstValue()) {
            setup.configFilePath = configFileArg.firstValue();
        }
        setup.run();
    });
    HelpArgument helpArg(parser);
    NoColorArgument noColorArg;
    parser.setMainArguments({ &runArg, &noColorArg, &helpArg });
    parser.setDefaultArgument(&runArg);
    parser.parseArgs(argc, argv);
    return 0;
}
