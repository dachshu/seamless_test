#include <iostream>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <string>
#include "Client.h"

#define MAX_BUFFER 1024
#define ID_PSIZE_SZ	(sizeof(int)*2)
int zone_server_id = 0;

boost::asio::io_context io_context;
tcp::socket front_sock(io_context);

int next_wid = 0;
int id_to_wid[MAX_CLIENT];

thread_local std::unordered_map<int, CLIENT*> worker_clients;

const int proxy_tid = NUM_WORKER_THREADS;
std::unordered_map<int, PROXY_CLIENT*> proxy_clients;

const unsigned int zone_width = int(WORLD_WIDTH / ZONE_SIZE);
const unsigned int zone_heigt = int((WORLD_HEIGHT / 2) / ZONE_SIZE);
Zone zone[zone_heigt][zone_width];

void get_zone_col_row(short x, short y, int& col, int& row)
{
	col = int(x / ZONE_SIZE);
	if (0 == zone_server_id) {
		row = int(y / ZONE_SIZE);
		row = row - int(row / zone_heigt);
	}
	else {
		row = int(y / ZONE_SIZE) - zone_heigt;
		if (row < 0) row = 0;
	}
}

void get_new_zone(std::set<int>& new_zone, short x, short y) {
	short x1 = x - VIEW_RANGE;
	if (x1 < 0) x1 = 0;
	short x2 = (x + VIEW_RANGE);
	if (x2 >= WORLD_WIDTH) x2 = WORLD_WIDTH - 1;
	short y1 = (y - VIEW_RANGE);
	if (y1 < 0) y1 = 0;
	short y2 = y + VIEW_RANGE;
	if (y2 >= WORLD_HEIGHT) y2 = WORLD_HEIGHT - 1;

	int zc, zr;
	get_zone_col_row(x1, y1, zc, zr);
	new_zone.insert(zc * zone_width + zr);
	get_zone_col_row(x1, y2, zc, zr);
	new_zone.insert(zc * zone_width + zr);
	get_zone_col_row(x2, y1, zc, zr);
	new_zone.insert(zc * zone_width + zr);
	get_zone_col_row(x2, y2, zc, zr);
	new_zone.insert(zc * zone_width + zr);
}

bool is_near(short x1, short y1, short x2, short y2)
{
	if (VIEW_RANGE < abs(x1 - x2)) return false;
	if (VIEW_RANGE < abs(y1 - y2)) return false;
	return true;
}

void send_packet(int id)
{
	CLIENT* cli = worker_clients[id];
	if (cli->prev_packet_size <= 0)
		return;

	int* p_size = reinterpret_cast<int*>(&cli->send_buf[0]);
	int* p_id = reinterpret_cast<int*>(&cli->send_buf[sizeof(int)]);
	int packet_size = cli->prev_packet_size + ID_PSIZE_SZ;
	*p_size = packet_size;
	*p_id = cli->id;
	
	boost::system::error_code ec;
	
	front_sock.write_some(boost::asio::buffer(cli->send_buf, packet_size), ec);
	cli->prev_packet_cnt = 0;
	cli->prev_packet_size = 0;
	if (ec)
		std::cout << "send error" << std::endl;
}

void send_defer(int id)
{
	CLIENT* cli = worker_clients[id];
	if (cli->prev_packet_cnt >= MAX_GATHER_SIZE)
		send_packet(id);
}

void send_put_object_packet(int id, int new_id, int nx, int ny, int o_type)
{
	CLIENT* cli = worker_clients[id];

	sc_packet_put_object* packet 
		= reinterpret_cast<sc_packet_put_object*>(&(cli->send_buf[ID_PSIZE_SZ + cli->prev_packet_size]));

	packet->id = new_id;
	packet->size = sizeof(sc_packet_put_object);
	packet->type = SC_PUT_OBJECT;
	packet->x = nx;
	packet->y = ny;
	packet->o_type = o_type;

	++(cli->prev_packet_cnt);
	cli->prev_packet_size += sizeof(sc_packet_put_object);
	
	send_defer(id);
}

void send_pos_packet(int id, int mover, int mx, int my, unsigned mv_time)
{
	CLIENT* cli = worker_clients[id];
	
	sc_packet_pos* packet 
		= reinterpret_cast<sc_packet_pos*>(&(cli->send_buf[ID_PSIZE_SZ + cli->prev_packet_size]));

	packet->id = mover;
	packet->size = sizeof(sc_packet_pos);
	packet->type = SC_POS;
	packet->x = mx;
	packet->y = my;
	packet->move_time = mv_time;

	++(cli->prev_packet_cnt);
	cli->prev_packet_size += sizeof(sc_packet_pos);

	send_defer(id);
}

void send_remove_object_packet(int id, int leaver)
{
	CLIENT* cli = worker_clients[id];
	
	sc_packet_remove_object* packet 
		= reinterpret_cast<sc_packet_remove_object*>((&cli->send_buf[ID_PSIZE_SZ + cli->prev_packet_size]));

	packet->id = leaver;
	packet->size = sizeof(sc_packet_remove_object);
	packet->type = SC_REMOVE_OBJECT;

	++(cli->prev_packet_cnt);
	cli->prev_packet_size += sizeof(sc_packet_remove_object);

	send_defer(id);
}


void handle_accept_client(MsgNode* msg)
{
	CLIENT* new_cli = new CLIENT;
	new_cli->id = msg->to;
	new_cli->my_woker_id = tid;
	new_cli->x = msg->x;
	new_cli->y = msg->y;
	new_cli->move_time = 0;
	new_cli->is_connected = true;

	new_cli->prev_packet_cnt = 0;
	new_cli->prev_packet_size = 0;
	int* p_id = reinterpret_cast<int*>(&new_cli->send_buf[sizeof(int)]);
	*p_id = new_cli->id;
	
	new_cli->zone_node_buffer.set(new_cli->my_woker_id, new_cli->id);

	//local_clients[new_cli->id] = new_cli;
	worker_clients.insert(std::make_pair(new_cli->id, new_cli));

	int user_id = new_cli->id;
	// 1. zone in
	int zc, zr;
	get_zone_col_row(new_cli->x, new_cli->y, zc, zr);
	ZoneNode* zn = new_cli->zone_node_buffer.get();
	zn->cid = user_id; zn->worker_id = tid;
	zone[zr][zc].Add(zn);

	new_cli->my_zone_col = zc;
	new_cli->my_zone_row = zr;

	// 2. broadcast
	std::set<int> new_zone;
	get_new_zone(new_zone, new_cli->x, new_cli->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % zone_width][i / zone_width].Broadcast(tid, user_id, Msg::MOVE
			, new_cli->x, new_cli->y, zone_server_id);
	}
	new_cli->broadcast_zone.swap(new_zone);
}

void handle_disconnect_msg(MsgNode* msg)
{
	int id = msg->to;
	CLIENT* cli = worker_clients[id];
	//1. zone ���� ����
	zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);

	//2. broadcast
	for (auto i : cli->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::BYE, -1, -1, -1);
	}

	//local_clients[id]->is_connected = false;
	delete cli;
	worker_clients.erase(id);

}

void handle_client_move_msg(MsgNode* msg)
{
	short x = msg->x;
	short y = msg->y;
	int id = msg->to;
	CLIENT* cli = worker_clients[id];
	cli->move_time = (size_t)msg->info;
	cli->x = x;
	cli->y = y;

	// 1. zone in/out
	int zc, zr;
	get_zone_col_row(x, y, zc, zr);

	if (zc != cli->my_zone_col || zr != cli->my_zone_row) {
		ZoneNode* zn = cli->zone_node_buffer.get();
		zn->cid = id; zn->worker_id = tid;
		zone[zr][zc].Add(zn);
		zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);
		cli->my_zone_col = zc;
		cli->my_zone_row = zr;
	}

	// 2. broadcast
	for (auto i : cli->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, zone_server_id);
	}

	std::set<int> new_zone;
	get_new_zone(new_zone, x, y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		if (cli->broadcast_zone.count(i) == 0) {
			zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, zone_server_id);
		}
	}
	cli->broadcast_zone.swap(new_zone);
}

void handle_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	CLIENT* cli = worker_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		// list�� �ְ� ������ send_put_packet
		cli->view_list.insert(msg->from_id);

		send_put_object_packet(my_id, msg->from_id, msg->x, msg->y, msg->o_type);
		
		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, zone_server_id);
		return;
	}

	// 2. �˴� ��
	if (in_view && (cli->view_list.count(msg->from_id) != 0)) {
		// �׳� ������ send_pos_packet
		send_pos_packet(my_id, msg->from_id, msg->x, msg->y, cli->move_time);
		return;
	}

	// 3. ������� ��
	if (!in_view && (cli->view_list.count(msg->from_id) != 0)) {
		// list���� ���� send_remove_packet
		cli->view_list.erase(msg->from_id);
		send_remove_object_packet(my_id, msg->from_id);
		// ������׵� BYE �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_id, -1);
		return;
	}
}

void handle_hi_msg(MsgNode* msg) {
	int my_id = msg->to;
	CLIENT* cli = worker_clients[my_id];
	int mx = cli->x;
	int my = cli->y;
	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		// list�� �ְ� ������ send_put_packet
		cli->view_list.insert(msg->from_id);
		send_put_object_packet(my_id, msg->from_id, msg->x, msg->y, msg->o_type);
	}
}

void handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	CLIENT* cli = worker_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	if (cli->view_list.count(msg->from_id) != 0) {
		cli->view_list.erase(msg->from_id);
		send_remove_object_packet(my_id, msg->from_id);
	}
}

void handle_client_leave_msg(MsgNode* msg) {
	int id = msg->to;
	CLIENT* cli = worker_clients[id];
	//1. zone ���� ����
	zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);

	msgQueue[NUM_WORKER_THREADS].Enq(tid, id, Msg::L_TO_P, msg->x, msg->y, msg->to, zone_server_id);
	//local_clients[id]->is_connected = false;
	delete cli;
	worker_clients.erase(id);
}

void handle_proxy_to_local_msg(MsgNode* msg) {
	CLIENT* new_cli = new CLIENT;
	new_cli->id = msg->to;
	new_cli->my_woker_id = tid;
	new_cli->x = msg->x;
	new_cli->y = msg->y;
	new_cli->move_time = 0;
	new_cli->is_connected = true;

	new_cli->prev_packet_cnt = 0;
	new_cli->prev_packet_size = 0;
	int* p_id = reinterpret_cast<int*>(&new_cli->send_buf[sizeof(int)]);
	*p_id = new_cli->id;

	new_cli->zone_node_buffer.set(new_cli->my_woker_id, new_cli->id);

	//local_clients[new_cli->id] = new_cli;
	worker_clients.insert(std::make_pair(new_cli->id, new_cli));

	int user_id = new_cli->id;
	// 1. zone in
	int zc, zr;
	get_zone_col_row(new_cli->x, new_cli->y, zc, zr);
	ZoneNode* zn = new_cli->zone_node_buffer.get();
	zn->cid = user_id; zn->worker_id = tid;
	zone[zr][zc].Add(zn);

	new_cli->my_zone_col = zc;
	new_cli->my_zone_row = zr;

	// 2. broadcast
	std::set<int> new_zone;
	get_new_zone(new_zone, new_cli->x, new_cli->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % zone_width][i / zone_width].Broadcast(tid, user_id, Msg::SERVER_MOVE
			, new_cli->x, new_cli->y, zone_server_id);
	}
	new_cli->broadcast_zone.swap(new_zone);
}

void handle_server_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	CLIENT* cli = worker_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		// list�� �ְ� ������ send_put_packet
		cli->view_list.insert(msg->from_id);

		send_put_object_packet(my_id, msg->from_id, msg->x, msg->y, msg->o_type);

		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, zone_server_id);
		return;
	}

	// 2. �˴� ��
	if (in_view && (cli->view_list.count(msg->from_id) != 0)) {
		// �׳� ������ send_pos_packet
		send_put_object_packet(my_id, msg->from_id, msg->x, msg->y, msg->o_type);
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, zone_server_id);
		return;
	}

	// 3. ������� ��
	if (!in_view && (cli->view_list.count(msg->from_id) != 0)) {
		// list���� ���� send_remove_packet
		cli->view_list.erase(msg->from_id);
		send_remove_object_packet(my_id, msg->from_id);
		// ������׵� BYE �����ֱ�
		//msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_id);
		return;
	}
}


void do_worker(int t)
{
	tid = t;
	while (true) {

		int checked_queue = 0;
		while (checked_queue++ < MAX_DQ_REUSLTS * 16)
		{
			MsgNode* msg = msgQueue[tid].Deq();
			if (msg == nullptr) break;

			if (msg->msg == Msg::ACCEPT) {
				handle_accept_client(msg);
				continue;
			}
			if (msg->msg == Msg::P_TO_L) {
				handle_proxy_to_local_msg(msg);
				continue;
			}

			if (worker_clients.count(msg->to) == 0) {
				continue;
			}

			switch (msg->msg)
			{
			case Msg::DISCONNECT:
				handle_disconnect_msg(msg);
				break;
			case Msg::CLI_MOVE:
				handle_client_move_msg(msg);
				break;
			case Msg::MOVE:
				handle_move_msg(msg);
				break;
			case Msg::HI:
				handle_hi_msg(msg);
				break;
			case Msg::BYE:
				handle_bye_msg(msg);
				break;
			case Msg::CLI_LEAVE:
				handle_client_leave_msg(msg);
				break;
			case Msg::SERVER_MOVE:
				handle_server_move_msg(msg);
				break;
			default:
				std::cout << "UNKOWN MSG" << std::endl;
				break;
			}
		}

		for (auto& cli : worker_clients) {
			if (cli.second->prev_packet_cnt > 0)
				send_packet(cli.first);
		}
	}
}


void proxy_handle_enter_msg(MsgNode* msg)
{
	PROXY_CLIENT* new_cli = new PROXY_CLIENT;
	new_cli->id = msg->to;
	new_cli->x = msg->x;
	new_cli->y = msg->y;
	new_cli->move_time = 0;
	new_cli->is_connected = true;

	new_cli->zone_node_buffer.set(proxy_tid, new_cli->id);

	proxy_clients.insert(std::make_pair(new_cli->id, new_cli));

	int user_id = new_cli->id;

	// 1. zone in
	int zc, zr;
	get_zone_col_row(new_cli->x, new_cli->y, zc, zr);
	ZoneNode* zn = new_cli->zone_node_buffer.get();
	zn->cid = user_id; zn->worker_id = tid;
	zone[zr][zc].Add(zn);

	new_cli->my_zone_col = zc;
	new_cli->my_zone_row = zr;

	// 2. broadcast
	std::set<int> new_zone;
	get_new_zone(new_zone, new_cli->x, new_cli->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % zone_width][i / zone_width].Broadcast(tid, user_id, Msg::MOVE
			, new_cli->x, new_cli->y, (zone_server_id + 1) % 2);
	}
	new_cli->broadcast_zone.swap(new_zone);
}

void proxy_handle_leave_msg(MsgNode* msg)
{
	int id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[id];
	//1. zone ���� ����
	zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);

	//2. broadcast
	for (auto i : cli->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::BYE, -1, -1, -1);
	}

	//local_clients[id]->is_connected = false;
	delete cli;
	proxy_clients.erase(id);

}

void proxy_handle_prx_move_msg(MsgNode* msg)
{
	short x = msg->x;
	short y = msg->y;
	int id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[id];
	cli->move_time = (size_t)msg->info;
	cli->x = x;
	cli->y = y;

	// 1. zone in/out
	int zc, zr;
	get_zone_col_row(x, y, zc, zr);

	if (zc != cli->my_zone_col || zr != cli->my_zone_row) {
		ZoneNode* zn = cli->zone_node_buffer.get();
		zn->cid = id; zn->worker_id = tid;
		zone[zr][zc].Add(zn);
		zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);
		cli->my_zone_col = zc;
		cli->my_zone_row = zr;
	}

	// 2. broadcast
	for (auto i : cli->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, (zone_server_id + 1) % 2);
	}

	std::set<int> new_zone;
	get_new_zone(new_zone, x, y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		if (cli->broadcast_zone.count(i) == 0) {
			zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, (zone_server_id + 1) % 2);
		}
	}
	cli->broadcast_zone.swap(new_zone);
}

void proxy_handle_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		cli->view_list.insert(msg->from_id);

		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, (zone_server_id + 1) % 2);
		return;
	}

	// 3. ������� ��
	if (!in_view && (cli->view_list.count(msg->from_id) != 0)) {
		cli->view_list.erase(msg->from_id);
		// ������׵� BYE �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_id, -1);
		return;
	}
}

void proxy_handle_hi_msg(MsgNode* msg) {
	int my_id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[my_id];
	int mx = cli->x;
	int my = cli->y;
	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		// list�� �ְ� ������ send_put_packet
		cli->view_list.insert(msg->from_id);
	}
}

void proxy_handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	if (cli->view_list.count(msg->from_id) != 0) {
		cli->view_list.erase(msg->from_id);
	}
}

void proxy_handle_client_enter_msg(MsgNode* msg) {
	int id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[id];
	//1. zone ���� ����
	zone[cli->my_zone_row][cli->my_zone_col].Remove(tid, id, cli->zone_node_buffer);

	msgQueue[msg->from_wid].Enq(tid, id, Msg::P_TO_L, msg->x, msg->y, msg->to, (zone_server_id + 1) % 2);
	//local_clients[id]->is_connected = false;
	delete cli;
	proxy_clients.erase(id);
}

void proxy_handle_proxy_to_local_msg(MsgNode* msg) {
	PROXY_CLIENT* new_cli = new PROXY_CLIENT;
	new_cli->id = msg->to;
	new_cli->x = msg->x;
	new_cli->y = msg->y;
	new_cli->move_time = 0;
	new_cli->is_connected = true;

	new_cli->zone_node_buffer.set(tid, new_cli->id);

	//local_clients[new_cli->id] = new_cli;
	proxy_clients.insert(std::make_pair(new_cli->id, new_cli));

	int user_id = new_cli->id;
	// 1. zone in
	int zc, zr;
	get_zone_col_row(new_cli->x, new_cli->y, zc, zr);
	ZoneNode* zn = new_cli->zone_node_buffer.get();
	zn->cid = user_id; zn->worker_id = tid;
	zone[zr][zc].Add(zn);

	new_cli->my_zone_col = zc;
	new_cli->my_zone_row = zr;

	// 2. broadcast
	std::set<int> new_zone;
	get_new_zone(new_zone, new_cli->x, new_cli->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % zone_width][i / zone_width].Broadcast(tid, user_id, Msg::SERVER_MOVE
			, new_cli->x, new_cli->y, (zone_server_id + 1) % 2);
	}
	new_cli->broadcast_zone.swap(new_zone);
}

void proxy_handle_server_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	PROXY_CLIENT* cli = proxy_clients[my_id];
	int mx = cli->x;
	int my = cli->y;

	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (cli->view_list.count(msg->from_id) == 0)) {
		// list�� �ְ� ������ send_put_packet
		cli->view_list.insert(msg->from_id);

		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, (zone_server_id + 1) % 2);
		return;
	}
	if (in_view) {
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_id, (zone_server_id + 1) % 2);
		return;
	}

	// 3. ������� ��
	if (!in_view && (cli->view_list.count(msg->from_id) != 0)) {
		// list���� ���� send_remove_packet
		cli->view_list.erase(msg->from_id);
		// ������׵� BYE �����ֱ�
		//msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_id);
		return;
	}
}

void proxy_worker()
{
	tid = proxy_tid;
	while (true) {

		int checked_queue = 0;
		while (checked_queue++ < MAX_DQ_REUSLTS * 16)
		{
			MsgNode* msg = msgQueue[tid].Deq();
			if (msg == nullptr) break;

			if (msg->msg == Msg::PRX_ENTER) {
				proxy_handle_enter_msg(msg);
				continue;
			}
			if (msg->msg == Msg::L_TO_P) {
				proxy_handle_proxy_to_local_msg(msg);
				continue;
			}
			if (proxy_clients.count(msg->to) == 0) {
				continue;
			}
			switch (msg->msg)
			{
			case Msg::PRX_LEAVE:
				proxy_handle_leave_msg(msg);
				break;
			case Msg::PRX_MOVE:
				proxy_handle_prx_move_msg(msg);
				break;
			case Msg::MOVE:
				proxy_handle_move_msg(msg);
				break;
			case Msg::HI:
				proxy_handle_hi_msg(msg);
				break;
			case Msg::BYE:
				proxy_handle_bye_msg(msg);
				break;
			case Msg::CLI_ENTER:
				proxy_handle_client_enter_msg(msg);
				break;
			case Msg::SERVER_MOVE:
				proxy_handle_server_move_msg(msg);
				break;
			default:
				std::cout << "UNKOWN MSG" << std::endl;
				break;
			}
		}
	}
}


void ProcessPacket(void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case FZ_ACCEPT: {
		fz_packet_accpet* accept_packet = reinterpret_cast<fz_packet_accpet*>(packet);
		//std::cout << "recv fz accept" << std::endl;
		next_wid = (++next_wid) % NUM_WORKER_THREADS;
		int wid = next_wid;
		id_to_wid[accept_packet->id] = wid;
		msgQueue[wid].Enq(-1, -1, Msg::ACCEPT, accept_packet->x, accept_packet->y, accept_packet->id, -1);
		break;
	}
	case FZ_DISCONNECT: {
		fz_packet_disconnect* discon_packet = reinterpret_cast<fz_packet_disconnect*>(packet);
		msgQueue[id_to_wid[discon_packet->id]].Enq(-1, -1, Msg::DISCONNECT, -1, -1, discon_packet->id, -1 );
		id_to_wid[discon_packet->id] = -1;
		//std::cout << "recv fz disconnect" << std::endl;
		break;
	}
	case FZ_CLIENT_MOVE: {
		fz_packet_client_move* move_packet = reinterpret_cast<fz_packet_client_move*>(packet);
		msgQueue[id_to_wid[move_packet->id]].Enq(-1, -1, Msg::CLI_MOVE
			, move_packet->x, move_packet->y, move_packet->id, -1, (void*)move_packet->move_time);
		//std::cout << "recv client move" << std::endl;
		break;
	}
	case FZ_CLIENT_LEAVE: {
		fz_packet_client_leave* leave_packet = reinterpret_cast<fz_packet_client_leave*>(packet);
		msgQueue[id_to_wid[leave_packet->id]].Enq(-1, -1, Msg::CLI_LEAVE, leave_packet->x, leave_packet->y, leave_packet->id, -1);
		id_to_wid[leave_packet->id] = proxy_tid;
		//std::cout << "recv client leave" << std::endl;
		break;
	}
	case FZ_CLIENT_ENTER: {
		fz_packet_client_enter* enter_packet = reinterpret_cast<fz_packet_client_enter*>(packet);
		next_wid = (++next_wid) % NUM_WORKER_THREADS;
		int wid = next_wid;
		
		msgQueue[proxy_tid].Enq(wid, -1, Msg::CLI_ENTER, enter_packet->x, enter_packet->y, enter_packet->id, -1);
		id_to_wid[enter_packet->id] = wid;
		//std::cout << "recv client enter" << std::endl;
		break;
	}
	case FZ_PROXY_ENTER: {
		fz_packet_proxy_enter* enter_packet = reinterpret_cast<fz_packet_proxy_enter*>(packet);
		id_to_wid[enter_packet->id] = proxy_tid;
		msgQueue[proxy_tid].Enq(-1, -1, Msg::PRX_ENTER, enter_packet->x, enter_packet->y, enter_packet->id, -1);
		//std::cout << "recv proxy enter" << std::endl;
		break;
	}
	case FZ_PROXY_LEAVE: {
		fz_packet_proxy_leave* leave_packet = reinterpret_cast<fz_packet_proxy_leave*>(packet);
		msgQueue[proxy_tid].Enq(-1, -1, Msg::PRX_LEAVE, -1, -1, leave_packet->id, -1);
		id_to_wid[leave_packet->id] = -1;
		//std::cout << "recv proxy leave" << std::endl;
		break;
	}
	case FZ_PROXY_MOVE: {
		fz_packet_proxy_move* move_packet = reinterpret_cast<fz_packet_proxy_move*>(packet);
		msgQueue[proxy_tid].Enq(-1, -1, Msg::PRX_MOVE, move_packet->x, move_packet->y, move_packet->id, -1);
		//std::cout << "recv proxy move" << std::endl;
		break;
	}
	default:
		std::cout << "Invalid Packet Type Error\n";
		while (true);
		break;
	}
}

int handle_recv(char* recv_buf, int bytes, int prev_packet_size)
{
	char* p = recv_buf;
	int remain = bytes;
	int packet_size;
	int prev_p_size = prev_packet_size;

	if (prev_p_size == 0) packet_size = 0;
	else packet_size = recv_buf[0];

	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		int required = packet_size - prev_p_size;
		if (required <= remain) {
			ProcessPacket(p);
			remain -= required;
			p += packet_size;
			prev_p_size = 0;
			packet_size = 0;
		}
		else {
			memcpy(&recv_buf[prev_p_size], &p[prev_p_size], remain);
			prev_p_size += remain;
			remain = 0;
		}
	}

	return prev_p_size;
}

int main()
{
	// read a config file
	std::ifstream config_file("config.txt");
	std::string s;
	// 1. read zone server id
	config_file >> s;
	zone_server_id = std::atoi(s.c_str());
	// 2. read frone-end-server ip
	std::string ip;
	config_file >> ip;
	// 3. read frone-end-server port
	config_file >> s;
	int fes_port = std::atoi(s.c_str());
	// connect to front-end-server

	tcp::endpoint front_end_server_addr(boost::asio::ip::address::from_string(ip), fes_port);
	boost::asio::connect(front_sock, &front_end_server_addr);
	std::cout << "server[" << zone_server_id <<"] connect to front-end-server" << std::endl;
	
	char recv_buf[MAX_BUFFER];
	int prev_packet_size = 0;

	epoch.store(1);
	for (int r = 0; r < NUM_TOTAL_WORKERS; ++r)
		reservations[r] = 0xffffffffffffffff;

	msg_node_epoch.store(1);
	for (int r = 0; r < NUM_TOTAL_WORKERS; ++r)
		msg_node_reservations[r] = 0xffffffffffffffff;

	std::vector <std::thread> worker_threads;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) 
		worker_threads.emplace_back(do_worker, i);
	worker_threads.emplace_back(proxy_worker);

	tid = NUM_WORKER_THREADS + 1;

	while (true) {
		boost::system::error_code ec;
		size_t len = front_sock.read_some(
			boost::asio::buffer(&recv_buf[prev_packet_size], MAX_BUFFER - prev_packet_size), ec);
		if (ec == boost::asio::error::eof) break;
		else if (ec) {
			std::cerr << "recv error" << std::endl;
			while (true);
		}

		prev_packet_size = handle_recv(recv_buf, len, prev_packet_size);
	}

	for (auto& th : worker_threads) th.join();
}