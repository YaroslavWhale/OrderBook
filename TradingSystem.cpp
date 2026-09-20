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
#include <mutex>
#include <atomic>
#include <vector>
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
    std::string url = "https://api.binance.com/api/v3/depth?symbol=" + symbol +
                      "&limit=" + std::to_string(limit);
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

void run(std::string symbol, uint64_t depth) {
    signal(SIGINT, signal_handler);

    Multipliers mult = load_multiplier(symbol);
    OrderBook book(symbol, depth);
    book.set_price_multiplier(mult.price);
    book.set_volume_multiplier(mult.volume);

    websocket_endpoint endpoint;

    std::mutex buffer_mtx;
    std::vector<DepthUpdate> buffer;
    std::atomic<bool> snapshot_ready{false};
    std::atomic<bool> need_resnapshot{false};

    auto parse_update = [&](const json& jd) -> DepthUpdate {
        DepthUpdate update;
        update.firstUpdateId = jd["U"].get<uint64_t>();
        update.finalUpdateId = jd["u"].get<uint64_t>();

        for (const auto& item : jd["b"]) {
            double price = std::stod(item[0].get<std::string>());
            double volume = std::stod(item[1].get<std::string>());
            update.bids.push_back({
                static_cast<uint64_t>(price * book.get_price_multiplier()),
                static_cast<uint64_t>(volume * book.get_volume_multiplier())
            });
        }
        for (const auto& item : jd["a"]) {
            double price = std::stod(item[0].get<std::string>());
            double volume = std::stod(item[1].get<std::string>());
            update.asks.push_back({
                static_cast<uint64_t>(price * book.get_price_multiplier()),
                static_cast<uint64_t>(volume * book.get_volume_multiplier())
            });
        }
        return update;
    };

    auto msg_handler = [&](websocketpp::connection_hdl, client::message_ptr msg) {
        try {
            auto jd = json::parse(msg->get_payload());
            if (!jd.contains("e") || jd["e"] != "depthUpdate") return;

            DepthUpdate update = parse_update(jd);

            if (!snapshot_ready.load()) {
                std::lock_guard<std::mutex> lock(buffer_mtx);
                if (!snapshot_ready.load()) {
                    buffer.push_back(std::move(update));
                    return;
                }
            }

            try {
                book.applyUpdate(update);
            } catch (const std::exception& e) {
                std::cerr << "Runtime sync error: " << e.what() << std::endl;
                need_resnapshot.store(true);
                snapshot_ready.store(false);
                std::lock_guard<std::mutex> lock(buffer_mtx);
                buffer.clear();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing message: " << e.what() << std::endl;
        }
    };

    std::string symbol_lower = symbol;
    std::transform(symbol_lower.begin(), symbol_lower.end(),
                   symbol_lower.begin(), ::tolower);
    std::string ws_uri =
        "wss://stream.binance.com:9443/ws/" + symbol_lower + "@depth@100ms";

    int conn_id = endpoint.connect(ws_uri, msg_handler);
    if (conn_id == -1) {
        std::cerr << "Failed to connect to WebSocket" << std::endl;
        return;
    }

    auto do_snapshot = [&]() -> bool {
        try {
            SnapshotData snap = get_snapshot(symbol, mult.price, mult.volume,
                                             static_cast<int>(depth));
            book.snapshot(snap);

            std::lock_guard<std::mutex> lock(buffer_mtx);
            uint64_t lastId = book.get_last_update_id();

            for (auto& upd : buffer) {
                if (upd.finalUpdateId <= lastId) continue;
                if (upd.firstUpdateId > lastId + 1) {
                    std::cerr << "Gap during buffer drain, re-snapshot needed\n";
                    buffer.clear();
                    snapshot_ready.store(false);
                    return false;
                }
                try {
                    book.applyUpdate(upd);
                } catch (const std::exception& e) {
                    std::cerr << "Buffer apply error: " << e.what() << std::endl;
                    buffer.clear();
                    snapshot_ready.store(false);
                    return false;
                }
                lastId = upd.finalUpdateId;
            }
            buffer.clear();

            snapshot_ready.store(true);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Snapshot error: " << e.what() << std::endl;
            return false;
        }
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    while (running) {
        if (do_snapshot()) {
            std::cout << "Synchronized!" << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "OrderBook is running. Press Ctrl+C to stop.\n";

    while (running) {
        if (need_resnapshot.exchange(false)) {
            std::cout << "Re-synchronizing..." << std::endl;
            while (running) {
                if (do_snapshot()) {
                    std::cout << "Synchronized!" << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (book.is_initialized()) {
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
