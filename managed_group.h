#ifndef MANAGED_GROUP_H_
#define MANAGED_GROUP_H_

#include <mutex>
#include <list>
#include <string>
#include <utility>

#include "view.h"
#include "rdmc/connection.h"

namespace derecho {

template<typename T>
struct LockedQueue {
    private:
        std::mutex mutex;
        using lock_t = std::unique_lock<std::mutex>;
        std::list<T> underlying_list;
    public:
        struct LockedListAccess {
            private:
                lock_t lock;
            public:
                std::list<T> &access;
                LockedListAccess(std::mutex& m, std::list<T>& a) :
                    lock(m), access(a) {};
        };
        LockedListAccess locked() {
            return LockedListAccess{ mutex, underlying_list };
        }
};

class ManagedGroup {
    private:

        LockedQueue<tcp::socket> pending_joins;

        const int gms_port;

        /** A flag to signal background threads to shut down; set to true when the group is destroyed. */
        std::atomic<bool> thread_shutdown;
        /** Holds references to background threads, so that we can shut them down during destruction. */
        std::vector<std::thread> background_threads;

        /** Sends a joining node the new view that has been constructed to include it.*/
        static void commit_join(const View& new_view, tcp::socket& client_socket);

        bool has_pending_join(){
            return pending_joins.locked().access.size() > 0;
        }

        /** Assuming this node is the leader, handles a join request from a client.*/
        void receive_join(tcp::socket& client_socket);

        /** Starts a new Derecho group with this node as the only member, and initializes the GMS. */
        static View start_group(const ip_addr& my_ip);
        /** Joins an existing Derecho group, initializing this object to participate in its GMS. */
        static View join_existing(const ip_addr& leader_ip, const int leader_port);

        //Ken's helper methods
        static void deliver_in_order(const View& Vc, int Leader);
        static void await_meta_wedged(const View& Vc);
        static int await_leader_globalMin_ready(const View& Vc);
        static void ragged_edge_cleanup(View& Vc);

        /** "Main loop" of the GMS that checks for joins and failures and reacts to them. */
        void monitor_changes();

        /** Creates the SST and derecho_group for the current view, using the current view's member list.
         * The parameters are all the possible parameters for constructing derecho_group. */
        void setup_sst_and_rdmc(long long unsigned int buffer_size, long long unsigned int block_size,
                message_callback global_stability_callback, rdmc::send_algorithm type, unsigned int window_size);
        /** Sets up the SST and derecho_group for a new view, based on the settings in the current view
         * (and copying over the SST data from the current view). */
        void transition_sst_and_rdmc(View& newView, int whichFailed);

    public:
        View curr_view;

        /** Constructor, starts or joins a managed Derecho group.
         * If my_ip == leader_ip, starts a new group with this node as the leader.
         * The rest of the parameters are the parameters for the derecho_group that should
         * be constructed for communications within this managed group. */
        ManagedGroup(const int gms_port, const ip_addr& my_ip, const ip_addr& leader_ip,
                long long unsigned int _buffer_size, long long unsigned int _block_size,
                message_callback global_stability_callback, rdmc::send_algorithm _type = rdmc::BINOMIAL_SEND, unsigned int _window_size = 3);

        ~ManagedGroup();
        /** Causes this node to cleanly leave the group by setting itself to "failed." */
        void leave();

        /** Reports to the GMS that the given node has failed. */
        void report_failure(const ip_addr& who);
        /** Gets a reference to the current derecho_group for the group being managed.
         * Clients can use this to send and receive messages. */
        derecho_group<View::N>& current_derecho_group();

};

} /* namespace derecho */

#endif /* MANAGED_GROUP_H_ */