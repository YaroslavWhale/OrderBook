#include "OrderBookClient.hpp"
#include "OrderBook.hpp"
#include "TradingSystem.hpp"
#include "types.hpp"

#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <algorithm>
#include <cctype>

int main(int argc, char* argv[]) {

    std::string sym;
    int depth;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-sym" && i + 1 < argc) {
            sym = argv[++i];
            std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
        } else if (arg == "-depth" && i + 1 < argc) {
            depth = std::stoi(argv[++i]);
        }
    }

    try {
        run(sym, depth);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    /*OrderBookCore OrderBook(sym, depth); // & В параметрах конструктора?

    bool done = false;
    std::string input;
    websocket_endpoint endpoint;

    while (!done) {
        std::cout << "Enter Command: ";
        std::getline(std::cin, input);

        if (input == "quit") {
            done = true;
        } else if (input == "help") {
            std::cout
                << "\nCommand List:\n"
                << "connect <ws uri>\n"
                << "show <connection id>\n"
                << "help: Display this help text\n"
                << "quit: Exit the program\n"
                << std::endl;
        } else if (input.substr(0, 7) == "connect") {
            int id = endpoint.connect(input.substr(8));
            if (id != -1) {
                std::cout << "> Created connection with id " << id << std::endl;
            }
        } else if (input.substr(0, 4) == "show") {
            int id = std::stoi(input.substr(5));
            connection_metadata::ptr metadata = endpoint.get_metadata(id);
            if (metadata) {
                std::cout << *metadata << std::endl; //atoi был
            } else {
                std::cout << "> Unknown connection id " << id << std::endl;
            }
        } else {
            std::cout << "> Unrecognized Command" << std::endl;
        }
    }*/

    return 0;
}
