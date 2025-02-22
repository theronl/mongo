/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/core/null_deleter.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>
#include <cctype>
#include <fstream>
#include <iostream>
#include <pcrecpp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logv2_appender.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logv2/attribute_argument_set.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/console.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/linenoise.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/stdx/utility.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/exit.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

#ifdef _WIN32
#include <io.h>
#include <shlobj.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace std::literals::string_literals;
using namespace mongo;

bool gotInterrupted = false;
bool inMultiLine = false;
static AtomicWord<bool> atPrompt(false);  // can eval before getting to prompt

namespace {
const std::string kDefaultMongoHost = "127.0.0.1"s;
const std::string kDefaultMongoPort = "27017"s;
const std::string kDefaultMongoURL = "mongodb://"s + kDefaultMongoHost + ":"s + kDefaultMongoPort;

// Initialize the featureCompatibilityVersion server parameter since the mongo shell does not have a
// featureCompatibilityVersion document from which to initialize the parameter. The parameter is set
// to the latest version because there is no feature gating that currently occurs at the mongo shell
// level. The server is responsible for rejecting usages of new features if its
// featureCompatibilityVersion is lower.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersionLatest,
                                     ("EndStartupOptionSetup"))
(InitializerContext* context) {
    mongo::serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);
    return Status::OK();
}

// Initialize the testCommandsEnabled server parameter to true since the mongo shell does not have
// any test-only commands that could cause harm to the server, and it may be necessary to enable
// this to test certain features, for example through benchRun (see SERVER-40419).
MONGO_INITIALIZER_WITH_PREREQUISITES(EnableShellTestCommands, ("EndStartupOptionSetup"))
(InitializerContext* context) {
    setTestCommandsEnabled(true);
    return Status::OK();
}
const auto kAuthParam = "authSource"s;

/**
 * This throws away all log output while inside of a LoggingDisabledScope.
 */
class ShellConsoleAppender final : public logger::ConsoleAppender<logger::MessageEventEphemeral> {
    using Base = logger::ConsoleAppender<logger::MessageEventEphemeral>;
    friend class ShellBackend;

public:
    using Base::Base;

    Status append(const Event& event) override {
        auto lk = stdx::lock_guard(mx);
        if (!loggingEnabled)
            return Status::OK();
        return Base::append(event);
    }

    struct LoggingDisabledScope {
        LoggingDisabledScope() {
            disableLogging();
        }

        ~LoggingDisabledScope() {
            enableLogging();
        }
    };

private:
    static void enableLogging() {
        auto lk = stdx::lock_guard(mx);
        invariant(!loggingEnabled);
        loggingEnabled = true;
    }

    static void disableLogging() {
        auto lk = stdx::lock_guard(mx);
        invariant(loggingEnabled);
        loggingEnabled = false;
    }

    // This needs to use a mutex rather than an atomic bool because we need to ensure that no more
    // logging will happen once we return from disable().
    static inline Mutex mx = MONGO_MAKE_LATCH("ShellConsoleAppender::mx");
    static inline bool loggingEnabled = true;
};

/**
 * Logv2 equivalent of ShellConsoleAppender above. Sharing the lock and LoggingDisabledScope.
 */
class ShellBackend final : public boost::log::sinks::text_ostream_backend {
public:
    void consume(boost::log::record_view const& rec, string_type const& formatted_message) {
        auto lk = stdx::lock_guard(ShellConsoleAppender::mx);
        if (!ShellConsoleAppender::loggingEnabled)
            return;
        boost::log::sinks::text_ostream_backend::consume(rec, formatted_message);
    }
};

/**
 * Formatter to provide specialized formatting for logs from javascript engine
 */
class ShellFormatter final : private logv2::TextFormatter {
public:
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) {
        using namespace logv2;
        using boost::log::extract;

        if (extract<LogTag>(attributes::tags(), rec).get().has(LogTag::kJavascript)) {
            StringData message = extract<StringData>(attributes::message(), rec).get();
            const auto& attrs = extract<AttributeArgumentSet>(attributes::attributes(), rec).get();

            _buffer.clear();
            fmt::internal::vformat_to(_buffer, to_string_view(message), attrs._values);
            strm.write(_buffer.data(), _buffer.size());
        } else {
            logv2::TextFormatter::operator()(rec, strm);
        }
    }
};

}  // namespace

namespace mongo {

enum ShellExitCode : int {
    kDBException = 1,
    kInputFileError = -3,
    kEvalError = -4,
    kMongorcError = -5,
    kUnterminatedProcess = -6,
    kProcessTerminationError = -7,
};

Scope* shellMainScope;
}  // namespace mongo

bool isSessionTimedOut() {
    static Date_t previousCommandTime = Date_t::now();
    if (shellGlobalParams.idleSessionTimeout > Seconds(0)) {
        const Date_t now = Date_t::now();

        if (now > (previousCommandTime + shellGlobalParams.idleSessionTimeout)) {
            return true;
        }
        previousCommandTime = now;
    }
    return false;
}

void generateCompletions(const std::string& prefix, std::vector<std::string>& all) {
    if (prefix.find('"') != std::string::npos)
        return;

    try {
        BSONObj args = BSON("0" << prefix);
        shellMainScope->invokeSafe(
            "function callShellAutocomplete(x) {shellAutocomplete(x)}", &args, nullptr);
        BSONObjBuilder b;
        shellMainScope->append(b, "", "__autocomplete__");
        BSONObj res = b.obj();
        BSONObj arr = res.firstElement().Obj();

        BSONObjIterator i(arr);
        while (i.more()) {
            BSONElement e = i.next();
            all.push_back(e.String());
        }
    } catch (...) {
    }
}

void completionHook(const char* text, linenoiseCompletions* lc) {
    std::vector<std::string> all;
    generateCompletions(text, all);

    for (unsigned i = 0; i < all.size(); ++i)
        linenoiseAddCompletion(lc, (char*)all[i].c_str());
}

void shellHistoryInit() {
    Status res = linenoiseHistoryLoad(shell_utils::getHistoryFilePath().string().c_str());
    if (!res.isOK()) {
        error() << "Error loading history file: " << res;
    }
    linenoiseSetCompletionCallback(completionHook);
}

void shellHistoryDone() {
    Status res = linenoiseHistorySave(shell_utils::getHistoryFilePath().string().c_str());
    if (!res.isOK()) {
        error() << "Error saving history file: " << res;
    }
    linenoiseHistoryFree();
}
void shellHistoryAdd(const char* line) {
    if (line[0] == '\0')
        return;

    // dont record duplicate lines
    static std::string lastLine;
    if (lastLine == line)
        return;
    lastLine = line;

    // We don't want any .auth() or .createUser() shell helpers added, but we want to
    // be able to add things like `.author`, so be smart about how this is
    // detected by using regular expresions. This is so we can avoid storing passwords
    // in the history file in plaintext.
    static pcrecpp::RE hiddenHelpers(
        "\\.\\s*(auth|createUser|updateUser|changeUserPassword)\\s*\\(");
    // Also don't want the raw user management commands to show in the shell when run directly
    // via runCommand.
    static pcrecpp::RE hiddenCommands(
        "(run|admin)Command\\s*\\(\\s*{\\s*(createUser|updateUser)\\s*:");

    static pcrecpp::RE hiddenFLEConstructor(".*Mongo\\(([\\s\\S]*)secretAccessKey([\\s\\S]*)");
    if (!hiddenHelpers.PartialMatch(line) && !hiddenCommands.PartialMatch(line) &&
        !hiddenFLEConstructor.PartialMatch(line)) {
        linenoiseHistoryAdd(line);
    }
}

void killOps() {
    if (shellGlobalParams.nokillop)
        return;

    if (atPrompt.load())
        return;

    sleepmillis(10);  // give current op a chance to finish

    mongo::shell_utils::connectionRegistry.killOperationsOnAllConnections(
        !shellGlobalParams.autoKillOp);
}

void quitNicely(int sig) {
    shutdown(EXIT_CLEAN);
}

// the returned string is allocated with strdup() or malloc() and must be freed by calling free()
char* shellReadline(const char* prompt, int handlesigint = 0) {
    auto lds = ShellConsoleAppender::LoggingDisabledScope();
    atPrompt.store(true);

    char* ret = linenoise(prompt);
    if (!ret) {
        gotInterrupted = true;  // got ^C, break out of multiline
    }

    atPrompt.store(false);
    return ret;
}

void setupSignals() {
#ifndef _WIN32
    signal(SIGHUP, quitNicely);
#endif
    signal(SIGINT, quitNicely);
}

std::string getURIFromArgs(const std::string& arg,
                           const std::string& host,
                           const std::string& port) {
    if (host.empty() && arg.empty() && port.empty()) {
        // Nothing provided, just play the default.
        return kDefaultMongoURL;
    }

    if ((str::startsWith(arg, "mongodb://") || str::startsWith(arg, "mongodb+srv://")) &&
        host.empty() && port.empty()) {
        // mongo mongodb://blah
        return arg;
    }
    if ((str::startsWith(host, "mongodb://") || str::startsWith(host, "mongodb+srv://")) &&
        arg.empty() && port.empty()) {
        // mongo --host mongodb://blah
        return host;
    }

    // We expect a positional arg to be a plain dbname or plain hostname at this point
    // since we have separate host/port args.
    if ((arg.find('/') != std::string::npos) && (host.size() || port.size())) {
        std::cerr << "If a full URI is provided, you cannot also specify --host or --port"
                  << std::endl;
        quickExit(-1);
    }

    const auto parseDbHost = [port](const std::string& db, const std::string& host) -> std::string {
        // Parse --host as a connection string.
        // e.g. rs0/host0:27000,host1:27001
        const auto slashPos = host.find('/');
        const auto hasReplSet = (slashPos > 0) && (slashPos != std::string::npos);

        std::ostringstream ss;
        ss << "mongodb://";

        // Handle each sub-element of the connection string individually.
        // Comma separated list of host elements.
        // Each host element may be:
        // * /unix/domain.sock
        // * hostname
        // * hostname:port
        // If --port is specified and port is included in connection string,
        // then they must match exactly.
        auto start = hasReplSet ? slashPos + 1 : 0;
        while (start < host.size()) {
            // Encode each host component.
            auto end = host.find(',', start);
            if (end == std::string::npos) {
                end = host.size();
            }
            if ((end - start) == 0) {
                // Ignore empty components.
                start = end + 1;
                continue;
            }

            const auto hostElem = host.substr(start, end - start);
            if ((hostElem.find('/') != std::string::npos) && str::endsWith(hostElem, ".sock")) {
                // Unix domain socket, ignore --port.
                ss << uriEncode(hostElem);

            } else {
                auto colon = hostElem.find(':');
                if ((colon != std::string::npos) &&
                    (hostElem.find(':', colon + 1) != std::string::npos)) {
                    // Looks like an IPv6 numeric address.
                    const auto close = hostElem.find(']');
                    if ((hostElem[0] == '[') && (close != std::string::npos)) {
                        // Encapsulated already.
                        ss << '[' << uriEncode(hostElem.substr(1, close - 1), ":") << ']';
                        colon = hostElem.find(':', close + 1);
                    } else {
                        // Not encapsulated yet.
                        ss << '[' << uriEncode(hostElem, ":") << ']';
                        colon = std::string::npos;
                    }
                } else if (colon != std::string::npos) {
                    // Not IPv6 numeric, but does have a port.
                    ss << uriEncode(hostElem.substr(0, colon));
                } else {
                    // Raw hostname/IPv4 without port.
                    ss << uriEncode(hostElem);
                }

                if (colon != std::string::npos) {
                    // Have a port in our host element, verify it.
                    const auto myport = hostElem.substr(colon + 1);
                    if (port.size() && (port != myport)) {
                        std::cerr
                            << "connection string bears different port than provided by --port"
                            << std::endl;
                        quickExit(-1);
                    }
                    ss << ':' << uriEncode(myport);
                } else if (port.size()) {
                    ss << ':' << uriEncode(port);
                } else {
                    ss << ":27017";
                }
            }
            start = end + 1;
            if (start < host.size()) {
                ss << ',';
            }
        }

        ss << '/' << uriEncode(db);

        if (hasReplSet) {
            // Remap included replica set name to URI option
            ss << "?replicaSet=" << uriEncode(host.substr(0, slashPos));
        }

        return ss.str();
    };

    if (host.size()) {
        // --host provided, treat it as the connect string and get db from positional arg.
        return parseDbHost(arg, host);
    } else if (arg.size()) {
        // --host missing, but we have a potential host/db positional arg.
        const auto slashPos = arg.find('/');
        if (slashPos != std::string::npos) {
            // host/db pair.
            return parseDbHost(arg.substr(slashPos + 1), arg.substr(0, slashPos));
        }

        // Compatability formats.
        // * Any arg with a dot is assumed to be a hostname or IPv4 numeric address.
        // * Any arg with a colon followed by a digit assumed to be host or IP followed by port.
        // * Anything else is assumed to be a db.

        if (arg.find('.') != std::string::npos) {
            // Assume IPv4 or hostnameish.
            return parseDbHost("test", arg);
        }

        const auto colonPos = arg.find(':');
        if ((colonPos != std::string::npos) && ((colonPos + 1) < arg.size()) &&
            isdigit(arg[colonPos + 1])) {
            // Assume IPv4 or hostname with port.
            return parseDbHost("test", arg);
        }

        // db, assume localhost.
        return parseDbHost(arg, "127.0.0.1");
    }

    // --host empty, position arg empty, fallback on localhost without a dbname.
    return parseDbHost("", "127.0.0.1");
}

std::string finishCode(std::string code) {
    while (!shell_utils::isBalanced(code)) {
        inMultiLine = true;
        code += "\n";
        // cancel multiline if two blank lines are entered
        if (code.find("\n\n\n") != std::string::npos)
            return ";";
        char* line = shellReadline("... ", 1);
        if (gotInterrupted) {
            if (line)
                free(line);
            return "";
        }
        if (!line)
            return "";

        char* linePtr = line;
        while (str::startsWith(linePtr, "... "))
            linePtr += 4;

        code += linePtr;
        free(line);
    }
    return code;
}

bool execPrompt(mongo::Scope& scope, const char* promptFunction, std::string& prompt) {
    std::string execStatement = std::string("__promptWrapper__(") + promptFunction + ");";
    scope.exec("delete __prompt__;", "", false, false, false, 0);
    scope.exec(execStatement, "", false, false, false, 0);
    if (scope.type("__prompt__") == String) {
        prompt = scope.getString("__prompt__");
        return true;
    }
    return false;
}

/**
 * Edit a variable or input buffer text in an external editor -- EDITOR must be defined
 *
 * @param whatToEdit Name of JavaScript variable to be edited, or any text string
 */
static void edit(const std::string& whatToEdit) {
    // EDITOR may be defined in the JavaScript scope or in the environment
    std::string editor;
    if (shellMainScope->type("EDITOR") == String) {
        editor = shellMainScope->getString("EDITOR");
    } else {
        static const char* editorFromEnv = getenv("EDITOR");
        if (editorFromEnv) {
            editor = editorFromEnv;
        }
    }
    if (editor.empty()) {
        std::cout << "please define EDITOR as a JavaScript string or as an environment variable"
                  << std::endl;
        return;
    }

    // "whatToEdit" might look like a variable/property name
    bool editingVariable = true;
    for (const char* p = whatToEdit.c_str(); *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '.')) {
            editingVariable = false;
            break;
        }
    }

    std::string js;
    if (editingVariable) {
        // If "whatToEdit" is undeclared or uninitialized, declare
        int varType = shellMainScope->type(whatToEdit.c_str());
        if (varType == Undefined) {
            shellMainScope->exec("var " + whatToEdit, "(shell)", false, true, false);
        }

        // Convert "whatToEdit" to JavaScript (JSON) text
        if (!shellMainScope->exec(
                "__jsout__ = tojson(" + whatToEdit + ")", "tojs", false, false, false))
            return;  // Error already printed

        js = shellMainScope->getString("__jsout__");

        if (strstr(js.c_str(), "[native code]")) {
            std::cout << "can't edit native functions" << std::endl;
            return;
        }
    } else {
        js = whatToEdit;
    }

    // Pick a name to use for the temp file
    std::string filename;
    const int maxAttempts = 10;
    int i;
    for (i = 0; i < maxAttempts; ++i) {
        StringBuilder sb;
#ifdef _WIN32
        char tempFolder[MAX_PATH];
        GetTempPathA(sizeof tempFolder, tempFolder);
        sb << tempFolder << "mongo_edit" << time(0) + i << ".js";
#else
        sb << "/tmp/mongo_edit" << time(nullptr) + i << ".js";
#endif
        filename = sb.str();
        if (!::mongo::shell_utils::fileExists(filename))
            break;
    }
    if (i == maxAttempts) {
        std::cout << "couldn't create unique temp file after " << maxAttempts << " attempts"
                  << std::endl;
        return;
    }

    // Create the temp file
    FILE* tempFileStream;
    tempFileStream = fopen(filename.c_str(), "wt");
    if (!tempFileStream) {
        std::cout << "couldn't create temp file (" << filename << "): " << errnoWithDescription()
                  << std::endl;
        return;
    }

    // Write JSON into the temp file
    size_t fileSize = js.size();
    if (fwrite(js.data(), sizeof(char), fileSize, tempFileStream) != fileSize) {
        int systemErrno = errno;
        std::cout << "failed to write to temp file: " << errnoWithDescription(systemErrno)
                  << std::endl;
        fclose(tempFileStream);
        remove(filename.c_str());
        return;
    }
    fclose(tempFileStream);

    // Pass file to editor
    StringBuilder sb;
    sb << editor << " " << filename;
    int ret = [&] {
        auto lds = ShellConsoleAppender::LoggingDisabledScope();
        return ::system(sb.str().c_str());
    }();
    if (ret) {
        if (ret == -1) {
            int systemErrno = errno;
            std::cout << "failed to launch $EDITOR (" << editor
                      << "): " << errnoWithDescription(systemErrno) << std::endl;
        } else
            std::cout << "editor exited with error (" << ret << "), not applying changes"
                      << std::endl;
        remove(filename.c_str());
        return;
    }

    // The editor gave return code zero, so read the file back in
    tempFileStream = fopen(filename.c_str(), "rt");
    if (!tempFileStream) {
        std::cout << "couldn't open temp file on return from editor: " << errnoWithDescription()
                  << std::endl;
        remove(filename.c_str());
        return;
    }
    sb.reset();
    int bytes;
    do {
        char buf[1024];
        bytes = fread(buf, sizeof(char), sizeof buf, tempFileStream);
        if (ferror(tempFileStream)) {
            std::cout << "failed to read temp file: " << errnoWithDescription() << std::endl;
            fclose(tempFileStream);
            remove(filename.c_str());
            return;
        }
        sb.append(StringData(buf, bytes));
    } while (bytes);

    // Done with temp file, close and delete it
    fclose(tempFileStream);
    remove(filename.c_str());

    if (editingVariable) {
        // Try to execute assignment to copy edited value back into the variable
        const std::string code = whatToEdit + std::string(" = ") + sb.str();
        if (!shellMainScope->exec(code, "tojs", false, true, false)) {
            std::cout << "error executing assignment: " << code << std::endl;
        }
    } else {
        linenoisePreloadBuffer(sb.str().c_str());
    }
}

namespace {
bool mechanismRequiresPassword(const MongoURI& uri) {
    if (const auto authMechanisms = uri.getOption("authMechanism")) {
        constexpr std::array<StringData, 2> passwordlessMechanisms{"GSSAPI"_sd, "MONGODB-X509"_sd};
        const std::string& authMechanism = authMechanisms.get();
        for (const auto& mechanism : passwordlessMechanisms) {
            if (mechanism.toString() == authMechanism) {
                return false;
            }
        }
    }
    return true;
}
}  // namespace

int _main(int argc, char* argv[], char** envp) {
    registerShutdownTask([] {
        // NOTE: This function may be called at any time. It must not
        // depend on the prior execution of mongo initializers or the
        // existence of threads.
        ::killOps();
        ::shellHistoryDone();
    });

    setupSignalHandlers();
    setupSignals();

    logger::globalLogManager()->getGlobalDomain()->clearAppenders();
    logger::globalLogManager()->getGlobalDomain()->attachAppender(
        std::make_unique<ShellConsoleAppender>(
            std::make_unique<logger::MessageEventDetailsEncoder>()));

    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));

    mongo::shell_utils::RecordMyLocation(argv[0]);

    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    setGlobalServiceContext(ServiceContext::make());
    // TODO This should use a TransportLayerManager or TransportLayerFactory
    auto serviceContext = getGlobalServiceContext();
    transport::TransportLayerASIO::Options opts;
    opts.enableIPv6 = shellGlobalParams.enableIPv6;
    opts.mode = transport::TransportLayerASIO::Options::kEgress;

    serviceContext->setTransportLayer(
        std::make_unique<transport::TransportLayerASIO>(opts, nullptr));
    auto tlPtr = serviceContext->getTransportLayer();
    uassertStatusOK(tlPtr->setup());
    uassertStatusOK(tlPtr->start());

    // hide password from ps output
    redactPasswordOptions(argc, argv);

    ErrorExtraInfo::invariantHaveAllParsers();

    if (!mongo::serverGlobalParams.quiet.load())
        std::cout << mongoShellVersion(VersionInfoInterface::instance()) << std::endl;

    if (!shellGlobalParams.logV2) {
        logger::globalLogManager()
            ->getNamedDomain("javascriptOutput")
            ->attachAppender(std::make_unique<ShellConsoleAppender>(
                std::make_unique<logger::MessageEventUnadornedEncoder>()));
    } else {
        logger::globalLogManager()->getGlobalDomain()->clearAppenders();
        logger::globalLogManager()->getGlobalDomain()->attachAppender(
            std::make_unique<logger::LogV2Appender<logger::MessageEventEphemeral>>(
                &(lv2Manager.getGlobalDomain())));
        logger::globalLogManager()
            ->getNamedDomain("javascriptOutput")
            ->attachAppender(std::make_unique<logger::LogV2Appender<logger::MessageEventEphemeral>>(
                &lv2Manager.getGlobalDomain(), logv2::LogTag::kJavascript));

        auto consoleSink = boost::make_shared<boost::log::sinks::synchronous_sink<ShellBackend>>();
        consoleSink->set_filter(logv2::ComponentSettingsFilter(lv2Manager.getGlobalDomain(),
                                                               lv2Manager.getGlobalSettings()));
        consoleSink->set_formatter(ShellFormatter());

        consoleSink->locked_backend()->add_stream(
            boost::shared_ptr<std::ostream>(&logv2::Console::out(), boost::null_deleter()));

        consoleSink->locked_backend()->auto_flush();

        boost::log::core::get()->add_sink(std::move(consoleSink));
    }


    // Get the URL passed to the shell
    std::string& cmdlineURI = shellGlobalParams.url;

    // Parse the output of getURIFromArgs which will determine if --host passed in a URI
    MongoURI parsedURI;
    parsedURI = uassertStatusOK(MongoURI::parse(getURIFromArgs(
        cmdlineURI, str::escape(shellGlobalParams.dbhost), str::escape(shellGlobalParams.port))));

    // TODO: add in all of the relevant shellGlobalParams to parsedURI
    parsedURI.setOptionIfNecessary("compressors"s, shellGlobalParams.networkMessageCompressors);
    parsedURI.setOptionIfNecessary("authMechanism"s, shellGlobalParams.authenticationMechanism);
    parsedURI.setOptionIfNecessary("authSource"s, shellGlobalParams.authenticationDatabase);
    parsedURI.setOptionIfNecessary("gssapiServiceName"s, shellGlobalParams.gssapiServiceName);
    parsedURI.setOptionIfNecessary("gssapiHostName"s, shellGlobalParams.gssapiHostName);

    if (const auto authMechanisms = parsedURI.getOption("authMechanism")) {
        std::stringstream ss;
        ss << "DB.prototype._defaultAuthenticationMechanism = \""
           << str::escape(authMechanisms.get()) << "\";" << std::endl;
        mongo::shell_utils::dbConnect += ss.str();
    }

    if (const auto gssapiServiveName = parsedURI.getOption("gssapiServiceName")) {
        std::stringstream ss;
        ss << "DB.prototype._defaultGssapiServiceName = \"" << str::escape(gssapiServiveName.get())
           << "\";" << std::endl;
        mongo::shell_utils::dbConnect += ss.str();
    }

    if (!shellGlobalParams.nodb) {  // connect to db
        bool usingPassword = !shellGlobalParams.password.empty();

        if (mechanismRequiresPassword(parsedURI) &&
            (parsedURI.getUser().size() || shellGlobalParams.username.size())) {
            usingPassword = true;
        }

        if (usingPassword && parsedURI.getPassword().empty()) {
            if (!shellGlobalParams.password.empty()) {
                parsedURI.setPassword(stdx::as_const(shellGlobalParams.password));
            } else {
                parsedURI.setPassword(mongo::askPassword());
            }
        }

        if (parsedURI.getUser().empty() && !shellGlobalParams.username.empty()) {
            parsedURI.setUser(stdx::as_const(shellGlobalParams.username));
        }

        std::stringstream ss;
        if (mongo::serverGlobalParams.quiet.load()) {
            ss << "__quiet = true;" << std::endl;
        }

        ss << "db = connect( \"" << parsedURI.canonicalizeURIAsString() << "\");" << std::endl;

        if (shellGlobalParams.shouldRetryWrites || parsedURI.getRetryWrites()) {
            // If the --retryWrites cmdline argument or retryWrites URI param was specified, then
            // replace the global `db` object with a DB object started in a session. The resulting
            // Mongo connection checks its _retryWrites property.
            ss << "db = db.getMongo().startSession().getDatabase(db.getName());" << std::endl;
        }

        mongo::shell_utils::dbConnect += ss.str();
    }

    mongo::ScriptEngine::setConnectCallback(mongo::shell_utils::onConnect);
    mongo::ScriptEngine::setup();
    mongo::getGlobalScriptEngine()->setJSHeapLimitMB(shellGlobalParams.jsHeapLimitMB);
    mongo::getGlobalScriptEngine()->setScopeInitCallback(mongo::shell_utils::initScope);
    mongo::getGlobalScriptEngine()->enableJIT(!shellGlobalParams.nojit);
    mongo::getGlobalScriptEngine()->enableJavaScriptProtection(
        shellGlobalParams.javascriptProtection);

    auto poolGuard = makeGuard([] { ScriptEngine::dropScopeCache(); });

    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());
    shellMainScope = scope.get();

    if (shellGlobalParams.runShell && !mongo::serverGlobalParams.quiet.load())
        std::cout << "type \"help\" for help" << std::endl;

    // Load and execute /etc/mongorc.js before starting shell
    std::string rcGlobalLocation;
#ifndef _WIN32
    rcGlobalLocation = "/etc/mongorc.js";
#else
    wchar_t programDataPath[MAX_PATH];
    if (S_OK == SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programDataPath)) {
        rcGlobalLocation = str::stream()
            << toUtf8String(programDataPath) << "\\MongoDB\\mongorc.js";
    }
#endif
    if (!rcGlobalLocation.empty() && ::mongo::shell_utils::fileExists(rcGlobalLocation)) {
        if (!scope->execFile(rcGlobalLocation, false, true)) {
            std::cout << "The \"" << rcGlobalLocation << "\" file could not be executed"
                      << std::endl;
        }
    }

    if (!shellGlobalParams.script.empty()) {
        mongo::shell_utils::MongoProgramScope s;
        if (!scope->exec(shellGlobalParams.script, "(shell eval)", false, true, false)) {
            error() << "exiting with code " << static_cast<int>(kEvalError);
            return kEvalError;
        }
        scope->exec("shellPrintHelper( __lastres__ );", "(shell2 eval)", true, true, false);
    }

    for (size_t i = 0; i < shellGlobalParams.files.size(); ++i) {
        mongo::shell_utils::MongoProgramScope s;

        if (shellGlobalParams.files.size() > 1)
            std::cout << "loading file: " << shellGlobalParams.files[i] << std::endl;

        if (!scope->execFile(shellGlobalParams.files[i], false, true)) {
            severe() << "failed to load: " << shellGlobalParams.files[i];
            error() << "exiting with code " << static_cast<int>(kInputFileError);
            return kInputFileError;
        }

        // Check if the process left any running child processes.
        std::vector<ProcessId> pids = mongo::shell_utils::getRunningMongoChildProcessIds();

        if (!pids.empty()) {
            std::cout << "terminating the following processes started by "
                      << shellGlobalParams.files[i] << ": ";
            std::copy(pids.begin(), pids.end(), std::ostream_iterator<ProcessId>(std::cout, " "));
            std::cout << std::endl;

            if (mongo::shell_utils::KillMongoProgramInstances() != EXIT_SUCCESS) {
                severe() << "one more more child processes exited with an error during "
                         << shellGlobalParams.files[i];
                error() << "exiting with code " << static_cast<int>(kProcessTerminationError);
                return kProcessTerminationError;
            }

            bool failIfUnterminatedProcesses = false;
            const StringData code =
                "function() { return typeof TestData === 'object' && TestData !== null && "
                "TestData.hasOwnProperty('failIfUnterminatedProcesses') && "
                "TestData.failIfUnterminatedProcesses; }"_sd;
            shellMainScope->invokeSafe(code.rawData(), nullptr, nullptr);
            failIfUnterminatedProcesses = shellMainScope->getBoolean("__returnValue");

            if (failIfUnterminatedProcesses) {
                severe() << "exiting with a failure due to unterminated processes, "
                            "a call to MongoRunner.stopMongod(), ReplSetTest#stopSet(), or "
                            "ShardingTest#stop() may be missing from the test";
                error() << "exiting with code " << static_cast<int>(kUnterminatedProcess);
                return kUnterminatedProcess;
            }
        }
    }

    if (shellGlobalParams.files.size() == 0 && shellGlobalParams.script.empty())
        shellGlobalParams.runShell = true;

    bool lastLineSuccessful = true;
    if (shellGlobalParams.runShell) {
        mongo::shell_utils::MongoProgramScope s;
        // If they specify norc, assume it's not their first time
        bool hasMongoRC = shellGlobalParams.norc;
        std::string rcLocation;
        if (!shellGlobalParams.norc) {
#ifndef _WIN32
            if (getenv("HOME") != nullptr)
                rcLocation = str::stream() << getenv("HOME") << "/.mongorc.js";
#else
            if (getenv("HOMEDRIVE") != nullptr && getenv("HOMEPATH") != nullptr)
                rcLocation = str::stream()
                    << toUtf8String(_wgetenv(L"HOMEDRIVE")) << toUtf8String(_wgetenv(L"HOMEPATH"))
                    << "\\.mongorc.js";
#endif
            if (!rcLocation.empty() && ::mongo::shell_utils::fileExists(rcLocation)) {
                hasMongoRC = true;
                if (!scope->execFile(rcLocation, false, true)) {
                    severe() << "The \".mongorc.js\" file located in your home folder could not be "
                                "executed";
                    error() << "exiting with code " << static_cast<int>(kMongorcError);
                    return kMongorcError;
                }
            }
        }

        if (!hasMongoRC && isatty(fileno(stdin))) {
            std::cout
                << "Welcome to the MongoDB shell.\n"
                   "For interactive help, type \"help\".\n"
                   "For more comprehensive documentation, see\n\thttp://docs.mongodb.org/\n"
                   "Questions? Try the support group\n\thttp://groups.google.com/group/mongodb-user"
                << std::endl;
            File f;
            f.open(rcLocation.c_str(), false);  // Create empty .mongorc.js file
        }

        if (!shellGlobalParams.nodb && !mongo::serverGlobalParams.quiet.load() &&
            isatty(fileno(stdin))) {
            scope->exec(
                "shellHelper( 'show', 'startupWarnings' )", "(shellwarnings)", false, true, false);

            scope->exec(
                "shellHelper( 'show', 'freeMonitoring' )", "(freeMonitoring)", false, true, false);

            scope->exec("shellHelper( 'show', 'automationNotices' )",
                        "(automationnotices)",
                        false,
                        true,
                        false);

            scope->exec("shellHelper( 'show', 'nonGenuineMongoDBCheck' )",
                        "(nonGenuineMongoDBCheck)",
                        false,
                        true,
                        false);
        }

        shellHistoryInit();

        std::string prompt;
        int promptType;

        while (1) {
            inMultiLine = false;
            gotInterrupted = false;

            promptType = scope->type("prompt");
            if (promptType == String) {
                prompt = scope->getString("prompt");
            } else if ((promptType == Code) && execPrompt(*scope, "prompt", prompt)) {
            } else if (execPrompt(*scope, "defaultPrompt", prompt)) {
            } else {
                prompt = "> ";
            }

            char* line = shellReadline(prompt.c_str());

            char* linePtr = line;  // can't clobber 'line', we need to free() it later
            if (linePtr) {
                while (linePtr[0] == ' ')
                    ++linePtr;
                int lineLen = strlen(linePtr);
                while (lineLen > 0 && linePtr[lineLen - 1] == ' ')
                    linePtr[--lineLen] = 0;
            }

            if (!linePtr || (strlen(linePtr) == 4 && strstr(linePtr, "exit"))) {
                if (!mongo::serverGlobalParams.quiet.load())
                    std::cout << "bye" << std::endl;
                if (line)
                    free(line);
                break;
            }

            std::string code = linePtr;
            if (code == "exit" || code == "exit;") {
                free(line);
                break;
            }

            // Support idle session lifetime limits
            if (isSessionTimedOut()) {
                std::cout << "Idle Connection Timeout: Shell session has expired" << std::endl;
                if (line)
                    free(line);
                break;
            }

            if (code == "cls") {
                free(line);
                linenoiseClearScreen();
                continue;
            }

            if (code.size() == 0) {
                free(line);
                continue;
            }

            if (str::startsWith(linePtr, "edit ")) {
                shellHistoryAdd(linePtr);

                const char* s = linePtr + 5;  // skip "edit "
                while (*s && isspace(*s))
                    s++;

                edit(s);
                free(line);
                continue;
            }

            gotInterrupted = false;
            code = finishCode(code);
            if (gotInterrupted) {
                std::cout << std::endl;
                free(line);
                continue;
            }

            if (code.size() == 0) {
                free(line);
                break;
            }

            bool wascmd = false;
            {
                std::string cmd = linePtr;
                std::string::size_type firstSpace;
                if ((firstSpace = cmd.find(" ")) != std::string::npos)
                    cmd = cmd.substr(0, firstSpace);

                if (cmd.find("\"") == std::string::npos) {
                    try {
                        lastLineSuccessful =
                            scope->exec(std::string("__iscmd__ = shellHelper[\"") + cmd + "\"];",
                                        "(shellhelp1)",
                                        false,
                                        true,
                                        true);
                        if (scope->getBoolean("__iscmd__")) {
                            lastLineSuccessful =
                                scope->exec(std::string("shellHelper( \"") + cmd + "\" , \"" +
                                                code.substr(cmd.size()) + "\");",
                                            "(shellhelp2)",
                                            false,
                                            true,
                                            false);
                            wascmd = true;
                        }
                    } catch (std::exception& e) {
                        std::cout << "error2:" << e.what() << std::endl;
                        wascmd = true;
                        lastLineSuccessful = false;
                    }
                }
            }

            if (!wascmd) {
                try {
                    lastLineSuccessful = scope->exec(code.c_str(), "(shell)", false, true, false);
                    if (lastLineSuccessful) {
                        scope->exec(
                            "shellPrintHelper( __lastres__ );", "(shell2)", true, true, false);
                    }
                } catch (std::exception& e) {
                    std::cout << "error:" << e.what() << std::endl;
                    lastLineSuccessful = false;
                }
            }

            shellHistoryAdd(code.c_str());
            free(line);
        }

        shellHistoryDone();
    }

    return (lastLineSuccessful ? 0 : 1);
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    int returnCode;
    try {
        WindowsCommandLine wcl(argc, argvW, envpW);
        returnCode = _main(argc, wcl.argv(), wcl.envp());
    } catch (mongo::DBException& e) {
        severe() << "exception: " << e.what();
        error() << "exiting with code " << static_cast<int>(kDBException);
        returnCode = kDBException;
    }
    quickExit(returnCode);
}
#else   // #ifdef _WIN32
int main(int argc, char* argv[], char** envp) {
    int returnCode;
    try {
        returnCode = _main(argc, argv, envp);
    } catch (mongo::DBException& e) {
        severe() << "exception: " << e.what();
        error() << "exiting with code " << static_cast<int>(kDBException);
        returnCode = kDBException;
    }
    quickExit(returnCode);
}
#endif  // #ifdef _WIN32
