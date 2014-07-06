#include "lobbyinterface.h"
#include <QCoreApplication>

namespace asio = boost::asio;
namespace ip = asio::ip;

void NetworkHandler::connect(std::string host, unsigned int port) {
    logger.info("Connecting to uberserver on ", host, ":", port);
    resolver.async_resolve({ host, std::to_string(port) },
        [=](const boost::system::error_code& ec, ip::udp::resolver::iterator it){

        if(ec) {
            logger.error("Could not resolve host: ", ec.message());
            return;
        }
        socket.async_connect({ it->endpoint().address(), it->endpoint().port() },
            [=](const boost::system::error_code& ec){

            if(ec)
                logger.error("Could not connect to lobby server: ", ec.message());
            asio::async_read_until(socket, readBuf, '\n', boost::bind(&NetworkHandler::onRead, this, _1, _2));
        });
    });
}

// Called in the network thread.
void NetworkHandler::onRead(const boost::system::error_code& ec, std::size_t /* bytes */) {
    if(!ec) {
        std::istream is(&readBuf);
        std::string msg;
        std::getline(is, msg);
        QCoreApplication::postEvent(eventReceiver, new ReadEvent(msg));
        asio::async_read_until(socket, readBuf, '\n', boost::bind(&NetworkHandler::onRead, this, _1, _2));
    } else if(ec.value() != asio::error::basic_errors::operation_aborted) {
        logger.warning("Could not read data from uberserver: ", ec.message());
    }
}

void NetworkHandler::send(std::string msg) {
    service.post([=]{
        try {
            asio::write(socket, boost::asio::buffer(msg));
        } catch(boost::system::system_error e) {
            logger.warning("Could not send data to uberserver: ", e.what());
        }
    });
}

void NetworkHandler::disconnect() {
    service.post([=]{
        if(socket.is_open()) {
            logger.info("Disconnecting from uberserver.");
            socket.close();
        }
    });
}

void NetworkHandler::runService() {
    service.run();
}

NetworkHandler::NetworkHandler(QObject* eventReceiver, Logger& logger) : resolver(service), socket(service),
        eventReceiver(eventReceiver), logger(logger) {
    work = new asio::io_service::work(service);
    thread = boost::thread(boost::bind(&NetworkHandler::runService, this));
}

NetworkHandler::~NetworkHandler() {
    delete work;
    thread.join();
}
