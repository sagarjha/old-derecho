#include "derecho_group.h"
#include <algorithm>
#include <chrono>
#include <thread>

using std::vector;
using std::map;
using std::mutex;
using std::unique_lock;
using std::lock_guard;

namespace derecho {
  derecho_group::derecho_group (vector <int> _members, int node_rank, long long unsigned int _buffer_size, long long unsigned int _block_size, message_callback _global_stability_callback, rdmc::send_algorithm _type, unsigned int _window_size) {
    // copy the parameters
    members = _members;
    num_members = members.size();
    // find the member_index
    for (int i = 0; i < num_members; ++i) {
      if (members[i] == node_rank) {
	member_index = i;
	break;
      }
    }
    block_size = _block_size;
    buffer_size = _buffer_size;
    type = _type;
    window_size = _window_size;
    assert (window_size >= 1);

    global_stability_callback = _global_stability_callback;

    // initialize start, end and indexes
    start.resize(num_members);
    end.resize(num_members);
    for (int i = 0; i < num_members; ++i) {
      start[i] = end[i] = 0;
    }

    last_received_messages.resize(num_members, -1);
    // rotated list of members - used for creating n internal RDMC groups
    vector <uint32_t> rotated_members (num_members);

    // create num_members groups one at a time
    for (int i = 0; i < num_members; ++i) {
      /* members[i] is the sender for the i^th group
       * for now, we simply rotate the members vector to supply to create_group
       * even though any arrangement of receivers in the members vector is possible
       */
      // allocate buffer for the group
      std::unique_ptr<char[]> buffer(new char[buffer_size]);
      buffers.push_back (std::move (buffer));
      // create a memory region encapsulating the buffer
      std::shared_ptr<rdma::memory_region> mr = std::make_shared<rdma::memory_region>(buffers[i].get(), buffer_size);
      mrs.push_back (mr);
      for (int j = 0; j < num_members; ++j) {
	rotated_members[j] = (uint32_t) members[(i+j)%num_members];
      }
      // i is the group number
      // receive desination checks if the message will exceed the buffer length at current position in which case it returns the beginning position
      if (i == member_index) {
	rdmc::create_group(i, rotated_members, block_size, type,
			   [this, i](size_t length) -> rdmc::receive_destination {
			     return {mrs[i], (buffer_size-start[i] < length)? 0:(size_t)start[i]};
			   },
			   [this, i](char *data, size_t size) {
			     {
			       lock_guard <mutex> lock (msg_state_mtx);
			       last_received_messages[i]++;
			       locally_stable_messages[last_received_messages[i]*num_members + i] = {i, last_received_messages[i], (long long unsigned int) (data-buffers[i].get()), size};
			       vector<long long int>::iterator min_it = std::min_element(std::begin(last_received_messages), std::end(last_received_messages));
			       int index = std::distance(std::begin(last_received_messages), min_it);
			       long long int new_seq_num = (*min_it+1) * num_members + index-1;
			       if (new_seq_num > (*sst)[member_index].seq_num) {
				 (*sst)[member_index].seq_num = new_seq_num;
				 sst->put (offsetof (Row, seq_num), sizeof (new_seq_num));
				 // sst->put ();
			       }
			     }
			     // signal derecho thread
			     derecho_cv.notify_one();
			   },
			   [](boost::optional<uint32_t>){});
      }
      else {
	rdmc::create_group(i, rotated_members, block_size, type,
			   [this, i](size_t length) -> rdmc::receive_destination {
			     return {mrs[i], (buffer_size-start[i] < length)? 0:(size_t)start[i]};
			   },
			   [this, i](char *data, size_t size) {
			     unique_lock <mutex> lock (msg_state_mtx);
			     last_received_messages[i]++;
			     locally_stable_messages[last_received_messages[i]*num_members + i] = {i, last_received_messages[i], (long long unsigned int) (data-buffers[i].get()), size};
			     vector<long long int>::iterator min_it = std::min_element(std::begin(last_received_messages), std::end(last_received_messages));
			     int index = std::distance(std::begin(last_received_messages), min_it);
			     long long int new_seq_num = (*min_it + 1) * num_members + index-1;
			     if (new_seq_num > (*sst)[member_index].seq_num) {
			       (*sst)[member_index].seq_num = new_seq_num;
			       sst->put (offsetof (Row, seq_num), sizeof (new_seq_num));
			       // sst->put ();
			     }
			   },
			   [](boost::optional<uint32_t>){});
      }
    }
    
    // create the SST writes table
    sst = new sst::SST<Row, sst::Mode::Writes> (members, node_rank);
    for (int i = 0; i < num_members; ++i) {
      (*sst)[i].seq_num = -1;
      (*sst)[i].stable_num = -1;
      (*sst)[i].delivered_num = -1;
    }
    sst->put();
    sst->sync_with_members();

    auto stability_pred = [this] (const sst::SST <Row, sst::Mode::Writes> & sst) {
      return true;
    };
    auto stability_trig = [this] (sst::SST <Row, sst::Mode::Writes> & sst) {
      // compute the min of the seq_num
      long long int min_seq_num = sst[0].seq_num;
      for (int i = 0; i < num_members; ++i) {
	if (sst[i].seq_num < min_seq_num) {
	  min_seq_num = sst[i].seq_num;
	}
      }
      if (min_seq_num > sst[member_index].stable_num) {
	sst[member_index].stable_num = min_seq_num;
	sst.put (offsetof (Row, stable_num), sizeof (min_seq_num));
	// sst.put ();
      }
    };
    sst->predicates.insert (stability_pred, stability_trig, sst::PredicateType::RECURRENT);

    auto delivery_pred = [this] (const sst::SST <Row, sst::Mode::Writes> & sst) {
      return true;
    };
    auto delivery_trig = [this] (sst::SST <Row, sst::Mode::Writes> & sst) {
      unique_lock <mutex> lock (msg_state_mtx);
      // compute the min of the stable_num
      long long int min_stable_num = sst[0].stable_num;
      for (int i = 0; i < num_members; ++i) {
	if (sst[i].stable_num < min_stable_num) {
	  min_stable_num = sst[i].stable_num;
	}
      }

      if (!locally_stable_messages.empty()) {
	long long int least_undelivered_seq_num = locally_stable_messages.begin()->first;
	if (least_undelivered_seq_num <= min_stable_num) {
	  msg_info msg = locally_stable_messages.begin()->second;
	  global_stability_callback (msg.sender_id, msg.index, buffers[msg.sender_id].get() + msg.offset, msg.size);
	  if (msg.sender_id == member_index) {
	    end[member_index] = msg.offset + msg.size;
	    if (end[member_index] == buffer_size) {
	      end[member_index] = 0;
	    }
	  }
	  sst[member_index].delivered_num = least_undelivered_seq_num;
	  sst.put (offsetof (Row, delivered_num), sizeof (least_undelivered_seq_num));
	  // sst.put ();
	  locally_stable_messages.erase (locally_stable_messages.begin());
	}
      }
    };
    sst->predicates.insert (delivery_pred, delivery_trig, sst::PredicateType::RECURRENT);
    
    auto derecho_pred = [this] (const sst::SST <Row, sst::Mode::Writes> & sst) {
      long long int seq_num = next_message_to_deliver*num_members + member_index;
	for (int i = 0; i < num_members; ++i) {
	  if (sst[i].delivered_num < seq_num) {
	    return false;
	  }
	}
      return true;
    };
    auto derecho_trig = [this] (sst::SST <Row, sst::Mode::Writes> & sst) {
      derecho_cv.notify_one ();
      next_message_to_deliver++;
    };
    sst->predicates.insert (derecho_pred, derecho_trig, sst::PredicateType::RECURRENT);
    
    // start derecho thread
    std::thread derecho(&derecho_group::send_loop, this);
    derecho.detach();
  }

  void derecho_group::send_loop () {
    auto should_send = [&]() {
      if (pending_sends.empty ()) {
	return false;
      }
      msg_info msg = pending_sends.front ();
      if (last_received_messages[member_index] < msg.index-1) {
	return false;
      }
	
      for (int i = 0; i < num_members; ++i) {
	if ((*sst)[i].delivered_num < (msg.index-window_size)*num_members + member_index) {
	  return false;
	}
      }
      return true;
    };
    unique_lock <mutex> lock (msg_state_mtx);
    while (true) {
      derecho_cv.wait (lock, should_send);
      msg_info msg = pending_sends.front ();
      rdmc::send (member_index, mrs[member_index], msg.offset, msg.size);
      pending_sends.pop();
    }
  }

  char* derecho_group::get_position (long long unsigned int msg_size) {
    // if the size of the message is greater than the (buffer size)/window_size, then return null
    if (msg_size > buffer_size/window_size) {
      cout << "Can't send messages of size larger than the size of the circular buffer divided by the window size" << endl;
      return NULL;
    }
    if (start[member_index] == end[member_index]) {
      start[member_index] = end[member_index] = 0;
    }
    long long unsigned int my_start = start[member_index];
    long long unsigned int my_end = end[member_index];
    if (my_start < my_end) {
      if (my_end - my_start >= msg_size) {
	start[member_index] += msg_size;
	if (start[member_index] == buffer_size) {
	  start[member_index] = 0;
	}
	msg_info msg = {member_index, future_message_index++, my_start, msg_size};
	next_message = msg;
	return buffers[member_index].get() + my_start;
      }
      else {
	return NULL;
      }
    }
    else {
      if (buffer_size - my_start >= msg_size) {
	start[member_index] += msg_size;
	if (start[member_index] == buffer_size) {
	  start[member_index] = 0;
	}
	msg_info msg = {member_index, future_message_index++, my_start, msg_size};
	next_message = msg;
	return buffers[member_index].get() + my_start;
      }
      else if (my_end >= msg_size) {
	start[member_index] = msg_size;
	if (start[member_index] == buffer_size) {
	  start[member_index] = 0;
	}
	msg_info msg = {member_index, future_message_index++, 0, msg_size};
	next_message = msg;
	return buffers[member_index].get();
      }
      else {
	return NULL;
      }
    }
  }

  void derecho_group::send () {
    {
      lock_guard <mutex> lock (msg_state_mtx);
      assert (next_message);
      pending_sends.push (*next_message);
      next_message = boost::none;
    }
    derecho_cv.notify_one();
  }

  void derecho_group::sst_print () {
    cout << "Printing SST" << endl;
    for (int i = 0; i < num_members; ++i) {
      cout << (*sst)[i].seq_num << " " << (*sst)[i].stable_num << endl;
    }
  }
}

