#include "mokv/db_client.hpp"
#include <string>
#include <iostream>
using namespace std;

int main() {
    mokv::DBClient client;
    while (true) {
        std::string op, key, value;
        cin >> op >> key;
        if (op == "get") {
            auto rsp = client.Get(key, value);
            std::cout << "rsp = " << rsp << " value = " << value << endl;
        } else if (op == "sget") {
            auto rsp = client.SyncGet(key, value);
            std::cout << "rsp = " << rsp << " value = " << value << endl;
        } else if (op == "put") {
            cin >> value;
            auto rsp = client.Put(key, value);
            std::cout << "rsp = " << rsp << endl;
        } else if (op == "optget") {
            // cin >> key;
            size_t idx;
            cin >> idx;
            auto rsp = client.Get(key, value, idx);
            std::cout << "rsp = " << rsp << " value = " << value << std::endl;
        }
    }
}