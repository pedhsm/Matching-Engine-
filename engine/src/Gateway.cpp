#include "Gateway.h"
#include <iostream>
#include <stdexcept>

Gateway::Gateway(const std::string& pull_endpoint, const std::string& pub_endpoint)
    : ctx_(1),
      pull_(ctx_, zmq::socket_type::pull),
      pub_(ctx_, zmq::socket_type::pub),
      proc_([this](const std::string& msg) { this->publicar(msg); }) {
    
    try {
        pull_.bind(pull_endpoint);
        pub_.bind(pub_endpoint);
        std::cout << "Gateway inicializado:\n"
                  << "  PULL (Order Entry) em " << pull_endpoint << "\n"
                  << "  PUB  (Market Data) em " << pub_endpoint << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "Erro no ZMQ: " << e.what() << std::endl;
        throw;
    }
}

void Gateway::run() {
    std::cout << "Aguardando comandos..." << std::endl;
    
    while (true) {
        zmq::message_t req;
        auto recv_res = pull_.recv(req, zmq::recv_flags::none);
        
        if (!recv_res) {
            continue;
        }

        std::string command(static_cast<char*>(req.data()), req.size());
        
        // Remove quebras de linha caso venham no comando
        if (!command.empty() && command.back() == '\n') command.pop_back();
        if (!command.empty() && command.back() == '\r') command.pop_back();

        if (command == "quit" || command == "stop") {
            std::cout << "Gateway recebendo encerramento." << std::endl;
            break;
        }

        proc_.process_line(command);
    }
}

void Gateway::publicar(const std::string& linha) {
    // Adiciona newline para facilitar a leitura no cliente
    std::string msg = linha + "\n";
    zmq::message_t reply(msg.size());
    std::memcpy(reply.data(), msg.c_str(), msg.size());
    
    // PUB não bloqueia de forma prejudicial
    pub_.send(reply, zmq::send_flags::none);
}
