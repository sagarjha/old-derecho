#include <stddef.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "../logger.h"
#include "../managed_group.h"
#include "../rdmc/util.h"

using namespace std;

const int GMS_PORT = 12345;
const size_t message_size = 1000;
const size_t block_size = 1000;

uint32_t num_nodes, node_rank;
map<uint32_t, std::string> node_addresses;

const int num_messages = 250;
bool done = false;
shared_ptr<derecho::ManagedGroup> managed_group;

void stability_callback(int sender_id, long long int index, char* data, long long int size) {
    using namespace derecho;
    util::debug_log().log_event(stringstream() << "Global stability for message "
            << index << " from sender " << sender_id);
}

void persistence_callback(int sender_id, long long int index, char* data, long long int size) {
    using namespace derecho;
    util::debug_log().log_event(stringstream() << "Persistence complete for message "
            << index << " from sender " << sender_id);
    if(index == num_messages - 1 && sender_id == (int)num_nodes - 1) {
        cout << "Done" << endl;
        done = true;
    }
}

void send_messages(int count) {
    for(int i = 0; i < count; ++i) {
        char* buffer = managed_group->get_sendbuffer_ptr(message_size);
        while(!buffer) {
            buffer = managed_group->get_sendbuffer_ptr(message_size);
        }
        memset(buffer, rand() % 256, message_size);
        managed_group->send();
    }
}

int main(int argc, char* argv[]) {
    srand(time(nullptr));
    query_addresses(node_addresses, node_rank);
    num_nodes = node_addresses.size();
    //This won't work! We need to support starting up a member without doing global setup
//    derecho::ManagedGroup::global_setup(node_addresses, node_rank);
    string debug_log_filename = (std::stringstream() << "events_node" << node_rank << ".csv").str();
    string message_log_filename = (std::stringstream() << "data" << node_rank << ".dat").str();

    managed_group = make_shared<derecho::ManagedGroup>(
        message_log_filename, GMS_PORT, node_addresses, node_rank,
        message_size, derecho::CallbackSet{stability_callback, persistence_callback},
        block_size);

    send_messages(num_messages);
    while(!done) {
    }
    ofstream log_stream(debug_log_filename);
    managed_group->print_log(log_stream);
}


