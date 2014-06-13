#include "lobbyinterface.h"
#include "logger.h"
#include <QCoreApplication>
#include <QSysInfo>
#include <QWebFrame>
#include <QWebPage>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/crc.hpp>
#include <boost/chrono.hpp>
#include <curl/curl.h>
#include <fstream>
#include <ctime>
#include <deque>
#include <algorithm>
#if defined Q_OS_LINUX || defined Q_OS_MAC
    #include <unistd.h>
    #include <sys/stat.h> // chmod()
#endif
#ifdef Q_OS_WIN32
    #include <windows.h>
#endif
namespace fs = boost::filesystem;

LobbyInterface::LobbyInterface(QObject *parent, QWebFrame *frame) :
        QObject(parent), springHome(""), debugNetwork(false), debugCommands(false),
        network(this, logger), frame(frame) {
    logger.setEventReceiver(this);
    auto args = QCoreApplication::arguments();
    if (args.contains("-debug-all")) {
        debugNetwork = debugCommands = true;
        logger.setDebug(true);
    }
    if (args.contains("-debug-net")) {
        debugNetwork = true;
        logger.setDebug(true);
    }
    if (args.contains("-debug-cmd")) {
        debugCommands = true;
        logger.setDebug(true);
    }
    if (args.contains("-debug"))
        logger.setDebug(true);

    #if defined Q_OS_LINUX
        char buf[1024];
        if (readlink("/proc/self/exe", buf, 1024) >= 0) {
            executablePath = fs::path(buf).parent_path();
        } else {
            getcwd(buf, 1024);
            executablePath = fs::path(buf);
        }
    #elif defined Q_OS_WIN32
        wchar_t buf[1024];
        GetModuleFileName(NULL, buf, 1024);
        executablePath = fs::path(buf).parent_path();
    #elif defined Q_OS_MAC
        #error TODO: _NSGetExecutablePath()
    #endif
}

static void copyFile(const fs::path& from, const fs::path& to) {
    // Can't use due to a linking error, see http://tinyurl.com/p2tuaft
    //fs::copy_file(from, to);
    u_ifstream src(from, std::ios::binary);
    u_ofstream dst(to, std::ios::binary);
    dst << src.rdbuf();
}
void LobbyInterface::move(const fs::path& src, const fs::path& dst) {
    if (fs::is_regular_file(src)) {
        logger.debug("Moving prepackaged file: ", src, " => ", dst);
        boost::system::error_code ec;
        fs::rename(src, dst, ec);
        // rename() doesn't work across fs boundaries.
        if (ec != 0) {
            copyFile(src, dst);
            fs::remove(src);
        }
    } else if (fs::is_directory(src)) {
        fs::create_directories(dst);
        fs::directory_iterator end;
        for (fs::directory_iterator it(src); it != end; it++) {
            move(it->path(), dst / it->path().filename());
        }
        fs::remove(src);
    }
}

void LobbyInterface::init() {
    #if defined Q_OS_LINUX
        os = "Linux";
    #elif defined Q_OS_WIN32 // Defined on 64-bit Windows too.
        os = "Windows";
    #elif defined Q_OS_MAC
        os = "Mac";
    #else
        #error "Unknown target OS."
    #endif

    writeSpringHomeSetting(readSpringHomeSetting());
    if (springHomeSetting.empty()) {
        if (os == "Windows") {
            // This calls SHGetFolderPath(... CSIDL_PERSONAL ...), same as what Spring uses.
            springHome = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdWString() +
                L"/My Games/Spring";
        } else {
            // ~/.spring
            springHome = QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toStdWString() +
                L"/.spring";
        }
    } else if (springHomeSetting.is_absolute()) {
        springHome = springHomeSetting;
    } else {
        springHome = executablePath / springHomeSetting;
    }
    logger.info("springHome is ", springHome);

    try {
        const fs::path weblobbyDir = springHome / "weblobby";
        fs::create_directories(weblobbyDir / "engine");
        fs::create_directories(weblobbyDir / "pr-downloader");
        fs::create_directories(weblobbyDir / "logs");
        frame->page()->settings()->setLocalStoragePath(QString::fromStdWString(weblobbyDir.wstring() + L"/storage"));
        logger.setLogFile(weblobbyDir / "weblobby.log");

        auto args = QCoreApplication::arguments();
        int argIndex = args.indexOf("-prepackaged-data");
        if (argIndex >= 0 && argIndex + 1 < args.length()) {
            fs::path path = args[argIndex+1].toStdWString();
            // Check for trailing slash.
            if (path.filename() == ".")
                path = path.parent_path();
            if (fs::exists(path)) {
                fs::directory_iterator end;
                for (fs::directory_iterator it(path); it != end; it++) {
                    move(it->path(), springHome / it->path().filename());
                }
            } else {
                logger.error("Prepackaged data not found at ", path);
            }
        }
    } catch(fs::filesystem_error e) {
        logger.error("Creating folders failed with: ", e.what());
    }
}

QString LobbyInterface::getSpringHome() {
    return QString::fromStdWString(springHome.wstring());
}

QString LobbyInterface::readSpringHomeSetting() {
    try {
        u_ifstream in(executablePath / "swlrc");
        in.exceptions(std::ios::failbit);
        std::string str;
        std::getline(in, str);
        const std::string key = "springHome:";
        if (str.find(key) != 0)
            springHomeSetting = "";
        else
            springHomeSetting = toStdWString((str.substr(key.length())));
    } catch(...) {
        springHomeSetting = "";
    }
    return QString::fromStdWString(springHomeSetting.wstring());
}

void LobbyInterface::writeSpringHomeSetting(QString path) {
    springHomeSetting = path.toStdWString();
    u_ofstream out(executablePath / "swlrc");
    out << "springHome:" << toStdString(springHomeSetting.wstring()) << std::endl;
}

void LobbyInterface::connect(QString host, unsigned int port) {
    network.connect(host.toStdString(), port);
}

void LobbyInterface::disconnect() {
    network.disconnect();
}

void LobbyInterface::send(QString msg) {
    network.send(msg.toStdString());
}

bool LobbyInterface::event(QEvent* evt) {
    if (evt->type() == NetworkHandler::ReadEvent::TypeId) {
        auto readEvt = dynamic_cast<NetworkHandler::ReadEvent&>(*evt);
        evalJs("on_socket_get('" + escapeJs(readEvt.msg) + "')");
        return true;
    } else if (evt->type() == Logger::LogEvent::TypeId) {
        auto logEvt = dynamic_cast<Logger::LogEvent&>(*evt);
        if(logEvt.lev == Logger::level::error)
            evalJs("alert2('" + escapeJs(logEvt.msg) + "')");
        return true;
    } else if (evt->type() == ProcessRunner::ReadEvent::TypeId) {
        auto readEvt = dynamic_cast<ProcessRunner::ReadEvent&>(*evt);
        evalJs("commandStream('" + escapeJs(readEvt.cmd) + "', '" + escapeJs(readEvt.msg) + "')");
        return true;
    } else if (evt->type() == ProcessRunner::TerminateEvent::TypeId) {
        auto termEvt = dynamic_cast<ProcessRunner::TerminateEvent&>(*evt);
        evalJs("commandStream('exit', '" + escapeJs(termEvt.cmd) + "')");
        logger.info("Command finished: ", termEvt.cmd);
        if(processes.count(termEvt.cmd)) {
            processes.find(termEvt.cmd)->second.terminate();
            processes.erase(processes.find(termEvt.cmd));
        }
        return true;
    } else if (evt->type() == UnitsyncHandlerAsync::ResultEvent::TypeId) {
        auto resEvt = dynamic_cast<UnitsyncHandlerAsync::ResultEvent&>(*evt);
        evalJs("unitsyncResult('" + escapeJs(resEvt.id) + "', '" + escapeJs(resEvt.type) + "', '" + escapeJs(resEvt.res) + "')");
        return true;
    } else {
        return QObject::event(evt);
    }
}

void LobbyInterface::jsMessage(std::string source, int lineNumber, std::string message) {
    if (message.find("<TASSERVER>") != std::string::npos ||
            message.find("<LOCAL>") != std::string::npos) {
        if (debugNetwork)
            logger.debug(message);
    } else if (message.find("<CMD>") != std::string::npos) {
        if (debugCommands)
            logger.debug(message);
    } else {
        logger.warning(source, ":", lineNumber, " ", message);
    }
}

QString LobbyInterface::listDirs(QString path) {
    return listFilesPriv(path, true);
}

QString LobbyInterface::listFiles(QString path) {
    return listFilesPriv(path, false);
}

QString LobbyInterface::listFilesPriv(QString qpath, bool dirs) {
    std::list<std::wstring> files;
    fs::path path = qpath.toStdWString();

    if (fs::is_directory(path)) {
        fs::directory_iterator end_itr;
        for (fs::directory_iterator it(path); it != end_itr; ++it) {
            if (dirs && fs::is_directory(it->path())) {
                auto f = it->path().filename().wstring();
                files.push_back(f);
            } else if (!dirs && !fs::is_directory(it->path())) {
                auto f = it->path().filename().wstring();
                files.push_back(f);
            }
        }
    } else {
        logger.error("listFilesPriv(): not a directory: ", path);
    }
    std::wstring out;
    if (!files.empty()) {
        out += files.front();
        for (auto it = ++(files.begin()); it != files.end(); it++) {
            out += L"||" + *it;
        }
    }

    return QString::fromStdWString(out);
}

QString LobbyInterface::readFileLess(QString qpath, unsigned int lines) {
    // TODO: deque? Just read the file in the reverse order, geez.
    fs::path path = qpath.toStdWString();
    u_ifstream in(path);
    if (!in) {
        logger.warning("readFileLess(): Could not open ", path);
        return "";
    }
    std::deque<std::string> q;
    while (in.good()) {
        if (q.size() > lines)
            q.pop_front();
        std::string s;
        std::getline(in, s);
        q.push_back(std::move(s));
    }
    QString res;
    for (auto l : q)
        res += (l + "\n").c_str();
    return res;
}

int curl_debug(CURL* hnld, curl_infotype type, char* str, size_t size, void* plogger) {
    Logger& logger = *(Logger*)plogger;
    char buf[2048];
    size = size > 2047 ? 2047 : size;
    std::copy(str, str + size, buf);
    buf[size] = '\0';
    if (type == CURLINFO_TEXT)
        logger.debug(buf);
    else if (type == CURLINFO_HEADER_IN)
        logger.debug("in: ", buf);
    else if (type == CURLINFO_HEADER_OUT)
        logger.debug("out: ", buf);
    return 0;
}
size_t static write_data(void* buf, size_t size, size_t mult, void* file) {
    ((u_ofstream*)file)->write((const char*)buf, size * mult);
    return size * mult;
}
bool LobbyInterface::downloadFile(QString qurl, QString qtarget) {
    auto url = qurl.toStdString();
    fs::path target = qtarget.toStdWString();
    logger.debug("downloadFile(): ", url, " => ", target);
    auto handle = curl_easy_init();
    curl_slist* hlist = NULL;

    if (fs::exists(target)) {
        auto lastM_ = fs::last_write_time(target);
        auto lastM = std::gmtime(&lastM_);
        std::ostringstream ss;
        auto curLocale = std::locale();
        std::locale::global(std::locale("C"));
        char httpDate[50];
        std::strftime(httpDate, 50, "%a, %d %b %Y %H:%M:%S GMT", lastM);
        std::locale::global(curLocale);
        logger.debug("downloadFile(): last modified on ", httpDate);
        hlist = curl_slist_append(hlist, ("If-Modified-Since: " + std::string(httpDate)).c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hlist);
    }

    auto tempFile = fs::temp_directory_path();
    tempFile /= "weblobby_dl";
    u_ofstream fo(tempFile, std::ios::binary);
    if (!fo.is_open() || !fo.good()) {
        logger.error("downloadFile(): can't open file: ", tempFile);
        return false;
    }

    /*curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, curl_debug);
    curl_easy_setopt(handle, CURLOPT_DEBUGDATA, &logger);*/
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &fo);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);

    CURLcode err;
    if ((err = curl_easy_perform(handle)) != 0) {
        logger.error("downloadFile(): can't download file: ", url, " => ", target, ": ", curl_easy_strerror(err));
        return false;
    }
    if (hlist)
        curl_slist_free_all(hlist);
    curl_easy_cleanup(handle);
    if (fo.tellp() > 0) {
        fo.close();
		copyFile(tempFile, target);
        #if defined Q_OS_LINUX || defined Q_OS_MAC
            if (target.string().find("pr-downloader") != std::string::npos)
                chmod(target.c_str(), S_IRWXU | S_IRWXG | S_IROTH);
        #endif
        logger.debug("downloadFile(): finished.");
    } else {
        logger.debug("downloadFile(): not modified, using the cached version");
    }
    return true;
}

QObject* LobbyInterface::getUnitsync(QString qpath) {
    fs::path path = qpath.toStdWString();
    if (!unitsyncs.count(path)) {
        unitsyncs.insert(std::make_pair(path, UnitsyncHandler(this, logger, path)));
    }

    if (unitsyncs.find(path)->second.isReady())
        return &(unitsyncs.find(path)->second);
    else {
        logger.warning("Unitsync not loaded at ", path);
        unitsyncs.erase(unitsyncs.find(path));
        return NULL;
    }
}

QObject* LobbyInterface::getUnitsyncAsync(QString qpath) {
    fs::path path = qpath.toStdWString();
    if (!unitsyncs_async.count(path)) {
        unitsyncs_async.insert(std::make_pair(path, UnitsyncHandlerAsync(this, logger, path)));
    }

    if (unitsyncs_async.find(path)->second.startThread())
        return &(unitsyncs_async.find(path)->second);
    else {
        logger.warning("Unitsync not loaded at ", path);
        unitsyncs_async.erase(unitsyncs_async.find(path));
        return NULL;
    }
}

void LobbyInterface::createScript(QString qpath, QString script) {
    fs::path path = qpath.toStdWString();
    u_ofstream out(path);
    if (out.is_open() && out.good())
        out << script.toStdString();
    else
        logger.error("Could not create script at ", path);
}

void LobbyInterface::killCommand(QString qcmdName)
{
    auto cmdName = qcmdName.toStdString();
    logger.info("Killing command: ", cmdName);
    if(processes.count(cmdName)) {
        processes.find(cmdName)->second.terminate();
        processes.erase(processes.find(cmdName));
    }
}

void LobbyInterface::runCommand(QString qcmdName, QStringList cmd) {
    auto cmdName = qcmdName.toStdString();
    if(!processes.count(cmdName)) {
        std::vector<std::wstring> args;
        for(auto i : cmd)
            args.push_back(i.toStdWString());
        auto it = processes.insert(std::make_pair(cmdName, ProcessRunner(this, cmdName, args))).first;
        logger.info("Running command (", cmdName, "):\n", cmd.join(" ").toStdString());
        try {
            it->second.run();
        } catch(boost::system::system_error e) {
            logger.error("Cannot start command: ", cmdName, ": ", e.what());
            processes.erase(it);
        }
    }
}

void LobbyInterface::createUiKeys(QString qpath) {
    boost::system::error_code ec;
    fs::path path = qpath.toStdWString();
    if (!fs::exists(path, ec)) {
        logger.info("Creating empty uikeys: ", path);
        u_ofstream out(path);
    }
}

void LobbyInterface::deleteSpringSettings(QString qpath) {
    fs::path path = qpath.toStdWString();
    if (!qpath.endsWith("springsettings.cfg")) {
        logger.error("deleteSpringSettings(): not a spring settings file: ", path);
        return;
    }
    boost::system::error_code ec;
    fs::remove(path, ec);
    if (ec == 0)
        logger.info("Deleted spring settings: ", path);
    else
        logger.warning("Couldn't delete spring settings: ", path);
}

void LobbyInterface::writeToFile(QString path, QString line) {
    u_ofstream out(fs::path(path.toStdWString()), std::ios::app);
    out << line.toStdString() << std::endl;
}

void LobbyInterface::playSound(QString url) {
    #if defined Q_OS_WIN32 || defined Q_OS_MAC
        if (mediaPlayer.state() != QMediaPlayer::StoppedState)
            return;
        mediaPlayer.setMedia(QUrl(url));
        mediaPlayer.play();
    #elif defined Q_OS_LINUX
        runCommand("gstreamer_play", { QString::fromStdWString((executablePath /
            "gstreamer_play").wstring()) , url });
    #endif
}


/* TODO: replace with a function to get replay info because that's how
 * ReadFileMore() was used anyway.
public String ReadFileMore(final String logFile, final int numLines) {
    try {
        File f = new File(logFile);
        if (!f.exists()) {
            return "";
        }
        String out = "";
        String curLine;
        int curLineNum = 1;

        BufferedReader br = null;
        br = new BufferedReader(new FileReader(logFile));

        while ((curLine = br.readLine()) != null && curLineNum <= numLines ) {
            out += curLine;
            curLineNum++;
        }

        return out;
    }
    catch(Exception e) {
        e.printStackTrace();
        //echoJs( "Problem reading from log file ("+logFile+"): " + e.toString() );
        return "";
    }
}*/

void LobbyInterface::evalJs(const std::string& code) {
    frame->evaluateJavaScript(QString::fromStdString("__java_js_wrapper(function(){" + code + "}, this);"));
}

std::string LobbyInterface::escapeJs(const std::string& str) {
    std::string res = "";
    for(char c : str) {
        if(c == '\'') res += "\\'";
        else if(c == '\\') res += "\\\\";
        else if(c == '"') res += "\\\"";
        else if(c == '\n') res += "\\n";
        else if(c == '\r') res += "\\r";
        else res += c;
    }
    return res;
}

int LobbyInterface::sendSomePacket(QString host, unsigned int port, QString msg) {
    return -1;
    /* TODO: This is called from JS but the result is never used. The purpose of
     * learning the local port of a UDP socket is unclear.
    try {
        //int port = 90;

        byte[] message = messageString.getBytes();

        // Get the internet address of the specified host
        InetAddress address = InetAddress.getByName(host);

        // Initialize a datagram packet with data and address
        DatagramPacket packet = new DatagramPacket(message, message.length, address, port);

        // Create a datagram socket, send the packet through it, close it.
        //DatagramSocket dsocket = new DatagramSocket();
        dsocket.send(packet);
        System.out.println(""+dsocket.getLocalPort());
        return dsocket.getLocalPort();
        //dsocket.close();
    } catch (Exception e) {
        System.err.println(e);
        for(int i=0; i<e.getStackTrace().length; i++)
        {
            echoJs( e.getStackTrace()[i]+"" );
        }
    }
    return -1;
    */
}

unsigned int LobbyInterface::getUserID() {
    for(auto i : QNetworkInterface::allInterfaces()) {
        if((i.flags() & QNetworkInterface::IsUp) &&
                (i.flags() & QNetworkInterface::IsRunning) &&
                !(i.flags() & QNetworkInterface::IsLoopBack) &&
                !i.hardwareAddress().startsWith("00:00:00:00")) { // hax
            logger.debug("Using ", i.hardwareAddress().toStdString(), " mac address in getUserID()");
            std::string str = i.hardwareAddress().toStdString() + "lobby.springrts.com";
            boost::crc_32_type crc;
            // Do we include '\0'? Does it matter?
            crc.process_bytes(str.c_str(), str.size());
            return crc.checksum();
        }
    }
    return 0;
}
