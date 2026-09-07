#pragma once

#include <unordered_map> // std::map?
#include <string>
#include <memory>

class OrderBookCore{
private:
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> order_books;
    //mutex;
public:

};
