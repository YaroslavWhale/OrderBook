#include "TradingSystem.hpp"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cctype>
#include "OrderBook.hpp"
#include "OrderBookClient.hpp"

using json = nlohmann::json;

static volatile std::sig_atomic_t running = 1;

void signal_handler(int sig) {
    if (sig == SIGINT) running = 0;
}

Multipliers load_multiplier(const std::string& symbol) {
    std::string url = "https://api.binance.com/api/v3/exchangeInfo?symbol=" + symbol;
    cpr::Response r = cpr::Get(cpr::Url{url});
    if (r.status_code != 200) {
        throw std::runtime_error("Failed to fetch info for " + symbol +
                                 ", status: " + std::to_string(r.status_code));
    }
    auto json_data = json::parse(r.text);
    if (json_data["symbols"].empty()) {
        throw std::runtime_error("Symbol not found: " + symbol);
    }
    const auto& sym = json_data["symbols"][0];
    uint64_t price_mult = 1, volume_mult = 1;
    for (const auto& filter : sym["filters"]) {
        if (filter["filterType"] == "PRICE_FILTER") {
            double tick_size = std::stod(filter["tickSize"].get<std::string>());
            price_mult = static_cast<uint64_t>(1.0 / tick_size);
        } else if (filter["filterType"] == "LOT_SIZE") {
            double step_size = std::stod(filter["stepSize"].get<std::string>());
            volume_mult = static_cast<uint64_t>(1.0 / step_size);
        }
    }
    if (price_mult == 1 || volume_mult == 1) {
        throw std::runtime_error("Required filters not found for symbol: " + symbol);
    }
    return {price_mult, volume_mult};
}

SnapshotData get_snapshot(const std::string& symbol,
                          uint64_t price_mult,
                          uint64_t volume_mult,
                          int limit) {
    std::string url = "https://api.binance.com/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
    cpr::Response r = cpr::Get(cpr::Url{url});
    if (r.status_code != 200) {
        throw std::runtime_error("Failed to fetch depth: " + std::to_string(r.status_code));
    }
    auto json_data = json::parse(r.text);
    SnapshotData data;
    data.lastUpdateId = json_data["lastUpdateId"].get<uint64_t>();
    for (const auto& item : json_data["bids"]) {
        double price = std::stod(item[0].get<std::string>());
        double volume = std::stod(item[1].get<std::string>());
        data.bids.push_back({static_cast<uint64_t>(price * price_mult),
                             static_cast<uint64_t>(volume * volume_mult)});
    }
    for (const auto& item : json_data["asks"]) {
        double price = std::stod(item[0].get<std::string>());
        double volume = std::stod(item[1].get<std::string>());
        data.asks.push_back({static_cast<uint64_t>(price * price_mult),
                             static_cast<uint64_t>(volume * volume_mult)});
    }
    return data;
}

/*void run(std::string symbol, uint64_t depth) {
    signal(SIGINT, signal_handler);

    Multipliers mult = load_multiplier(symbol);
    SnapshotData snap = get_snapshot(symbol, mult.price, mult.volume, static_cast<int>(depth));

    OrderBook book(symbol, depth);
    book.set_price_multiplier(mult.price);
    book.set_volume_multiplier(mult.volume);
    book.snapshot(snap);

    websocket_endpoint endpoint;

    uint64_t update_counter = 0;
    auto last_update_time = std::chrono::steady_clock::now();

    auto msg_handler = [&book, &update_counter, &last_update_time](websocketpp::connection_hdl, client::message_ptr msg) {
        try {
            auto payload = msg->get_payload();
            std::cerr << "WebSocket message: " << payload << std::endl; // отладка
            auto json_data = json::parse(payload);
            if (json_data["e"] == "depthUpdate") {
                DepthUpdate update;
                update.firstUpdateId = json_data["U"].get<uint64_t>();
                update.finalUpdateId = json_data["u"].get<uint64_t>();

                for (const auto& item : json_data["b"]) {
                    double price = std::stod(item[0].get<std::string>());
                    double volume = std::stod(item[1].get<std::string>());
                    uint64_t price_int = static_cast<uint64_t>(price * book.get_price_multiplier());
                    uint64_t volume_int = static_cast<uint64_t>(volume * book.get_volume_multiplier());
                    update.bids.push_back({price_int, volume_int});
                }
                for (const auto& item : json_data["a"]) {
                    double price = std::stod(item[0].get<std::string>());
                    double volume = std::stod(item[1].get<std::string>());
                    uint64_t price_int = static_cast<uint64_t>(price * book.get_price_multiplier());
                    uint64_t volume_int = static_cast<uint64_t>(volume * book.get_volume_multiplier());
                    update.asks.push_back({price_int, volume_int});
                }
                book.applyUpdate(update);
                ++update_counter;
                last_update_time = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing WebSocket message: " << e.what() << std::endl;
        }
    };

    std::string symbol_lower = symbol;
    std::transform(symbol_lower.begin(), symbol_lower.end(), symbol_lower.begin(), ::tolower);
    std::string ws_uri = "wss://stream.binance.com:9443/ws/" + symbol_lower + "@depth@100ms";
    int conn_id = endpoint.connect(ws_uri, msg_handler);
    if (conn_id == -1) {
        std::cerr << "Failed to connect to WebSocket" << std::endl;
        return;
    }

    std::cout << "OrderBook is running. Press Ctrl+C to stop.\n";
    std::cout << "Connection ID: " << conn_id << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto meta = endpoint.get_metadata(conn_id);
    if (meta) {
        std::cout << "Initial connection status: " << *meta << std::endl;
    }

    while (running) {
        if (book.is_initialized()) {
            std::cout << "\n=== " << symbol << " Order Book ===\n";

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time).count();
            std::cout << "Last update: " << elapsed << " ms ago  |  Updates total: " << update_counter << "\n";

            auto bids = book.get_bids();
            auto asks = book.get_asks();

            int64_t bid = book.get_best_bid_price();
            int64_t ask = book.get_best_ask_price();
            int64_t spread = (ask > 0 && bid > 0) ? (ask - bid) : 0;

            std::cout << "Best Bid: " << bid << "  Volume: " << book.get_best_bid_volume() << "\n";
            std::cout << "Best Ask: " << ask << "  Volume: " << book.get_best_ask_volume() << "\n";
            std::cout << "Spread: " << spread << "\n\n";

            std::cout << "Bids (top 5):\n";
            size_t bid_count = std::min<size_t>(5, bids.size());
            for (size_t i = 0; i < bid_count; ++i) {
                std::cout << "  " << bids[i].price << "  ->  " << bids[i].volume << "\n";
            }

            std::cout << "\nAsks (top 5):\n";
            size_t ask_count = std::min<size_t>(5, asks.size());
            for (size_t i = 0; i < ask_count; ++i) {
                std::cout << "  " << asks[i].price << "  ->  " << asks[i].volume << "\n";
            }
        } else {
            std::cout << "Waiting for initial snapshot...\n";
        }

        auto meta_current = endpoint.get_metadata(conn_id);
        if (meta_current) {
            std::cout << "Connection status: " << *meta_current << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Shutting down...\n";
}*/

void run(std::string symbol, uint64_t depth) {
    signal(SIGINT, signal_handler);

    Multipliers mult = load_multiplier(symbol);
    OrderBook book(symbol, depth);
    book.set_price_multiplier(mult.price);
    book.set_volume_multiplier(mult.volume);

    websocket_endpoint endpoint;
    std::atomic<bool> synchronized{false};

    auto msg_handler = [&](websocketpp::connection_hdl, client::message_ptr msg) {
        try {
            auto json_data = json::parse(msg->get_payload());
            if (json_data["e"] == "depthUpdate") {
                DepthUpdate update;
                update.firstUpdateId = json_data["U"].get<uint64_t>();
                update.finalUpdateId = json_data["u"].get<uint64_t>();

                for (const auto& item : json_data["b"]) {
                    double price = std::stod(item[0].get<std::string>());
                    double volume = std::stod(item[1].get<std::string>());
                    uint64_t price_int = static_cast<uint64_t>(price * book.get_price_multiplier());
                    uint64_t volume_int = static_cast<uint64_t>(volume * book.get_volume_multiplier());
                    update.bids.push_back({price_int, volume_int});
                }
                for (const auto& item : json_data["a"]) {
                    double price = std::stod(item[0].get<std::string>());
                    double volume = std::stod(item[1].get<std::string>());
                    uint64_t price_int = static_cast<uint64_t>(price * book.get_price_multiplier());
                    uint64_t volume_int = static_cast<uint64_t>(volume * book.get_volume_multiplier());
                    update.asks.push_back({price_int, volume_int});
                }

                if (!book.is_initialized()) {
                    return;
                }

                try {
                    book.applyUpdate(update);
                    if (!synchronized.load()) {
                        synchronized.store(true);
                        std::cout << "Synchronized!" << std::endl;
                    }
                } catch (const std::exception& e) {
                    if (synchronized.load()) {
                        std::cerr << "Runtime sync error: " << e.what() << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing message: " << e.what() << std::endl;
        }
    };

    std::string symbol_lower = symbol;
    std::transform(symbol_lower.begin(), symbol_lower.end(), symbol_lower.begin(), ::tolower);
    std::string ws_uri = "wss://stream.binance.com:9443/ws/" + symbol_lower + "@depth@100ms";
    int conn_id = endpoint.connect(ws_uri, msg_handler);
    if (conn_id == -1) {
        std::cerr << "Failed to connect to WebSocket" << std::endl;
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    try {
        SnapshotData snap = get_snapshot(symbol, mult.price, mult.volume, static_cast<int>(depth));
        book.snapshot(snap);
    } catch (const std::exception& e) {
        std::cerr << "Snapshot error: " << e.what() << std::endl;
        return;
    }

    std::cout << "OrderBook is running. Press Ctrl+C to stop.\n";

    while (running) {
        if (book.is_initialized()) {
            auto bids = book.get_bids();
            auto asks = book.get_asks();
            std::cout << "\n=== " << symbol << " Order Book ===\n";
            std::cout << "Best Bid: " << book.get_best_bid_price()
                      << " Volume: " << book.get_best_bid_volume() << "\n";
            std::cout << "Best Ask: " << book.get_best_ask_price()
                      << " Volume: " << book.get_best_ask_volume() << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Shutting down...\n";
}
