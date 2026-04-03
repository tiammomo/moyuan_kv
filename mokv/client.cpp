#include "mokv/db_client.hpp"
#include <filesystem>
#include <getopt.h>
#include <string>
#include <iostream>
using namespace std;

namespace {

void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [-c <config_file>]\n"
              << "Commands:\n"
              << "  put <key> <value>\n"
              << "  get <key>\n"
              << "  sget <key>\n"
              << "  optget <key> <index>\n";
}

}

int main(int argc, char* argv[]) {
    std::filesystem::path config_file = mokv::raft::ConfigManager::DefaultPath();
    int opt = 0;
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
            case 'c':
                config_file = std::filesystem::absolute(optarg);
                break;
            case 'h':
                PrintUsage(argv[0]);
                return 0;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    std::unique_ptr<mokv::DBClient> client;
    try {
        client = std::make_unique<mokv::DBClient>(config_file);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
    while (true) {
        std::string op, key, value;
        cin >> op >> key;
        if (op == "get") {
            auto rsp = client->Get(key, value);
            std::cout << "rsp = " << rsp << " value = " << value << endl;
        } else if (op == "sget") {
            auto rsp = client->SyncGet(key, value);
            std::cout << "rsp = " << rsp << " value = " << value << endl;
        } else if (op == "put") {
            cin >> value;
            auto rsp = client->Put(key, value);
            std::cout << "rsp = " << rsp << endl;
        } else if (op == "optget") {
            // cin >> key;
            size_t idx;
            cin >> idx;
            auto rsp = client->Get(key, value, idx);
            std::cout << "rsp = " << rsp << " value = " << value << std::endl;
        }
    }
}
