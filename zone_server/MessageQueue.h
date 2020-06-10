#pragma once
#include <iostream>
#include <atomic>
#include "common.h"
#include "protocol.h"

#define INIT_MSG_SIZE MAX_CLIENT*128
#define INIT_MSG_CAPACITY int(INIT_MSG_SIZE*(1.5))

enum class Msg {
	HI, BYE, MOVE
	, SERVER_MOVE
	
	, ACCEPT, DISCONNECT, CLI_MOVE
	, CLI_LEAVE, P_TO_L

	, PRX_ENTER, PRX_LEAVE, PRX_MOVE
	, CLI_ENTER, L_TO_P
};

class MsgNode {
public:
	int from_wid;
	int from_id;
	Msg msg;
	short x;
	short y;
	int to;
	int o_type;
	void* info;

	unsigned long long retired_epoch{ 0 };
	MsgNode* next;

	MsgNode() { next = nullptr; }
	MsgNode(int wid, int id, Msg msg, short x, short y, int to, int o_type) :
		from_wid(wid), from_id(id), msg(msg), x(x), y(y), to(to), o_type(o_type) {
		next = nullptr;
	}
	~MsgNode() {}
};

std::atomic_ullong msg_node_reservations[NUM_TOTAL_WORKERS];
std::atomic_ullong msg_node_epoch = 1;
const unsigned int msg_node_epoch_freq = 1;

unsigned int get_min_msg_node_reservation() {
	unsigned long long min_re = 0xffffffffffffffff;
	for (int i = 0; i < NUM_TOTAL_WORKERS; ++i) {
		min_re = std::min(min_re, msg_node_reservations[i].load(std::memory_order_acquire));
	}
	return min_re;
}

void msg_start_op() {
	msg_node_reservations[tid].store(msg_node_epoch.load(std::memory_order_acquire), std::memory_order_release);
}

void msg_end_op() {
	msg_node_reservations[tid].store(0xffffffffffffffff, std::memory_order_release);
}

class MsgNodeBuffer {
	std::vector<MsgNode*> emptyNodes;
public:
	MsgNodeBuffer() {
		emptyNodes.reserve(INIT_MSG_CAPACITY);
		for (int i = 0; i < INIT_MSG_SIZE; ++i) {
			emptyNodes.emplace_back(new MsgNode);
		}
	}

	MsgNode* get() {
		if (emptyNodes.empty()) {
			//std::cout << "Msg node empty" << std::endl;
			return (new MsgNode);
		}

		unsigned int max_safe_epoch = get_min_msg_node_reservation();
		int idx = emptyNodes.size() - 1;
		int last = idx;
		for (; idx >= 0; --idx) {
			if (emptyNodes[idx]->retired_epoch < max_safe_epoch) {
				MsgNode* ret = emptyNodes[idx];
				emptyNodes[idx] = emptyNodes[last];
				emptyNodes.pop_back();

				ret->next = nullptr;
				return ret;
			}
		}

		return (new MsgNode);

	}


	void retire(MsgNode* msg) {
		msg->retired_epoch = msg_node_epoch.load(std::memory_order_acquire);

		emptyNodes.push_back(msg);

		//counter++;
		//if (counter % epoch_freq == 0)
		msg_node_epoch.fetch_add(1, std::memory_order_release);
	}
};

thread_local MsgNodeBuffer msgNodeBuffer;

// MPSC
class MessageQueue {
	MsgNode* volatile head;
	MsgNode* volatile tail;
public:

	MessageQueue()
	{
		head = tail = new MsgNode();
	}
	~MessageQueue() {}

	void Init()
	{
		MsgNode* ptr;
		while (head->next != nullptr) {
			ptr = head->next;
			head->next = head->next->next;
			delete ptr;
		}
		tail = head;
	}

	bool CAS(MsgNode* volatile* addr, MsgNode* old_node, MsgNode* new_node)
	{
		return std::atomic_compare_exchange_strong(reinterpret_cast<volatile std::atomic_llong*>(addr),
			reinterpret_cast<long long*>(&old_node),
			reinterpret_cast<long long>(new_node));
	}

	void Enq(int wid, int id, Msg msg, short x, short y, int to, int o_type
		, void* info = nullptr)
	{
		msg_start_op();
		MsgNode* e = msgNodeBuffer.get();
		e->from_wid = wid;
		e->from_id = id;
		e->msg = msg;
		e->x = x; e->y = y;
		e->to = to;
		e->o_type = o_type;
		e->info = info;


		while (true) {
			MsgNode* last = tail;
			MsgNode* next = last->next;
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&last->next, nullptr, e)) continue;
			CAS(&tail, last, e);
			msg_end_op();
			return;
		}
	}

	// ���� ���� thread�� retire �ؾ� �Ѵ�
	MsgNode* Deq()
	{
		msg_start_op();
		while (true) {
			MsgNode* first = head;
			MsgNode* next = first->next;
			MsgNode* last = tail;
			MsgNode* lastnext = last->next;

			if (last == first) {
				if (lastnext == nullptr) {
					msg_end_op();
					return nullptr;
				}
				else
				{
					CAS(&tail, last, lastnext);
					continue;
				}
			}
			if (nullptr == next) continue;

			head = next;
			//if (false == CAS(&head, first, next)) continue;
			first->next = nullptr;

			msgNodeBuffer.retire(first);
			msg_end_op();
			return head;
			//delete first;
		}
	}
};

MessageQueue msgQueue[NUM_TOTAL_WORKERS];