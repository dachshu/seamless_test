#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <boost/asio.hpp>

#include "protocol.h"

using boost::asio::ip::tcp;


#define NUM_WORKER_THREADS 4

#define NUM_ZONE_SERVER 2

#define LOCAL_SEND_BUF_CNT	int(MAX_CLIENT)


#define BOUNDARY_SIZE 4
#define BUFFER_SIZE 2
// 196, 201 
// 198, 205
short bounary[NUM_ZONE_SERVER][2] = { {int(WORLD_HEIGHT / 2 - (BOUNDARY_SIZE + BUFFER_SIZE)), int(WORLD_HEIGHT / 2 + BUFFER_SIZE - 1)}
									, {int(WORLD_HEIGHT / 2 - BUFFER_SIZE), int(WORLD_HEIGHT / 2 + (BOUNDARY_SIZE + BUFFER_SIZE - 1))} };



boost::asio::io_context io_context;
tcp::acceptor client_acceptor{ io_context, tcp::endpoint(tcp::v4(), FRONT_SERVER_PORT) };

void DisconnectClient(int id);
void ProcessClientPacket(int id, void* buf);
void ProcessServerPacket(int id, void* buf);

class CLIENT
{
public:
	int id;
	int zone_sid;

	tcp::socket sock{io_context};
	short x;
	short y;
	unsigned move_time;

	bool in_proxy;

	char recv_buf[MAX_PACKET_SIZE];
	int prev_packet_size;
	
	bool is_connected{ false };
	
public:
	//CLIENT(tcp::socket& s) : sock(std::move(s)) {}

	void do_read()
	{
		sock.async_read_some(boost::asio::buffer(&recv_buf[prev_packet_size], MAX_PACKET_SIZE - prev_packet_size),
			[this](boost::system::error_code ec, std::size_t length)
			{
				if (ec) {
					DisconnectClient(id);
					return;
				}
				char* p = recv_buf;
				int remain = length;
				int packet_size;
				int prev_p_size = prev_packet_size;

				if (prev_p_size == 0) packet_size = 0;
				else packet_size = recv_buf[0];

				while (remain > 0) {
					if (0 == packet_size) packet_size = p[0];
					int required = packet_size - prev_p_size;
					if (required <= remain) {
						ProcessClientPacket(id, p);
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

				prev_packet_size = prev_p_size;

				do_read();
			});
	}
};


const unsigned int zone_server_recv_buf_size = (sizeof(int) * 2 + MAX_PACKET_SIZE * MAX_GATHER_SIZE) * 100;

class ZONE_SERVER
{
public:
	int sid;
	tcp::socket sock{ io_context };

	char recv_buf[zone_server_recv_buf_size];
	int prev_packet_size;

public:
	void do_read()
	{
		sock.async_read_some(boost::asio::buffer(&recv_buf[prev_packet_size], (zone_server_recv_buf_size) - prev_packet_size),
			[this](boost::system::error_code ec, std::size_t length)
			{
				if (ec) {
					std::cout << "error in read a zone server sock" << std::endl;
					while (true);
					return;
				}
				
				char* p = recv_buf;
				int remain = length;
				int packet_size;
				int prev_p_size = prev_packet_size;
				int total_packet_size = prev_packet_size + length;

				if (prev_p_size == 0) packet_size = 0;
				else packet_size = *(reinterpret_cast<int*>(&recv_buf[0]));

				while (remain > 0) {
					if (0 == packet_size) packet_size = *(reinterpret_cast<int*>(&p[0]));
					int required = packet_size - prev_p_size;
					if (total_packet_size >= sizeof(int) && required <= remain) {
						ProcessServerPacket(sid, p);
						remain -= required;
						p += packet_size;
						total_packet_size -= packet_size;
						prev_p_size = 0;
						packet_size = 0;
					}
					else {
						memcpy(&recv_buf[prev_p_size], &p[prev_p_size], remain);
						prev_p_size += remain;
						remain = 0;
					}
				}

				prev_packet_size = prev_p_size;

				do_read();
			});
	}
};



class LocalSendBufferPool {
	std::vector<char*> sendBuffers;

public:
	LocalSendBufferPool() {
		sendBuffers.reserve(int(LOCAL_SEND_BUF_CNT * 1.5));
		for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
			char* buf = new char[MAX_PACKET_SIZE * MAX_GATHER_SIZE];
			sendBuffers.push_back(buf);
		}
	}

	char* get_sendBuffer() {
		if (sendBuffers.empty()) {
			//std::cout << "no more send buf" << std::endl;
			return new char[MAX_PACKET_SIZE * MAX_GATHER_SIZE];
		}
		
		char* buf = sendBuffers.back();
		sendBuffers.pop_back();
		return buf;
	}

	void return_sendBuffer(char* buf) {
		sendBuffers.push_back(buf);
	}
};

thread_local LocalSendBufferPool send_buf_pool;
CLIENT clients[MAX_CLIENT];
ZONE_SERVER zone_servers[NUM_ZONE_SERVER];

unsigned long fast_rand(void)
{ //period 2^96-1
	static thread_local unsigned long x = 123456789, y = 362436069, z = 521288629;
	unsigned long t;
	x ^= x << 16;
	x ^= x >> 5;
	x ^= x << 1;

	t = x;
	x = y;
	y = z;
	z = t ^ x ^ y;

	return z;
}

bool is_in_boundary(short y)
{
	if ((WORLD_HEIGHT / 2) - BOUNDARY_SIZE <= y && y < (WORLD_HEIGHT / 2) + BOUNDARY_SIZE)
		return true;
	return false;
}


void send_packet(int id, char* packet, int packet_size)
{
	//if (false == clients[id].is_connected) {
	//	send_buf_pool.return_sendBuffer(packet);
	//	return;
	//}

	boost::asio::async_write(clients[id].sock,
		boost::asio::buffer(packet, packet_size),
		[packet](boost::system::error_code ec, std::size_t length)
		{
			send_buf_pool.return_sendBuffer(packet);
		});
}

void send_login_ok_packet(int id)
{
	char* buf = send_buf_pool.get_sendBuffer();

	sc_packet_login_ok* packet = reinterpret_cast<sc_packet_login_ok*>(buf);

	packet->id = clients[id].id;
	packet->size = sizeof(sc_packet_login_ok);
	packet->type = SC_LOGIN_OK;
	packet->x = clients[id].x;
	packet->y = clients[id].y;
	packet->hp = 100;
	packet->level = clients[id].zone_sid;
	packet->exp = 1;

	send_packet(id, buf, sizeof(sc_packet_login_ok));
}

void send_put_object_packet(int client, int new_id)
{
	char* buf = send_buf_pool.get_sendBuffer();
	sc_packet_put_object* packet = reinterpret_cast<sc_packet_put_object*>(buf);

	packet->id = clients[new_id].id;
	packet->size = sizeof(sc_packet_put_object);
	packet->type = SC_PUT_OBJECT;
	packet->x = clients[new_id].x;
	packet->y = clients[new_id].y;
	packet->o_type = clients[new_id].zone_sid;
	
	send_packet(clients[client].id, buf, sizeof(sc_packet_put_object));
}

void send_pos_packet(int client, int mover)
{
	char* buf = send_buf_pool.get_sendBuffer();
	sc_packet_pos* packet = reinterpret_cast<sc_packet_pos*>(buf);
	packet->id = mover;
	packet->size = sizeof(sc_packet_pos);
	packet->type = SC_POS;
	packet->x = clients[mover].x;
	packet->y = clients[mover].y;
	packet->move_time = clients[mover].move_time;

	send_packet(clients[client].id, buf, sizeof(sc_packet_pos));
}

void send_packet_to_server(int sid, char* packet, int packet_size)
{
	boost::asio::async_write(zone_servers[sid].sock,
		boost::asio::buffer(packet, packet_size),
		[packet](boost::system::error_code ec, std::size_t length)
		{
			send_buf_pool.return_sendBuffer(packet);
		});
}

void send_accept_packet(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_accpet* packet = reinterpret_cast<fz_packet_accpet*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_accpet);
	packet->type = FZ_ACCEPT;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;

	send_packet_to_server(sid, buf, sizeof(fz_packet_accpet));
}

void send_disconnect_packet(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_disconnect* packet = reinterpret_cast<fz_packet_disconnect*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_disconnect);
	packet->type = FZ_DISCONNECT;

	send_packet_to_server(sid, buf, sizeof(fz_packet_disconnect));
}

void send_client_move(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_client_move* packet = reinterpret_cast<fz_packet_client_move*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_client_move);
	packet->type = FZ_CLIENT_MOVE;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;
	packet->move_time = clients[cid].move_time;

	send_packet_to_server(sid, buf, sizeof(fz_packet_client_move));
}

void send_client_leave(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_client_leave* packet = reinterpret_cast<fz_packet_client_leave*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_client_leave);
	packet->type = FZ_CLIENT_LEAVE;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;

	send_packet_to_server(sid, buf, sizeof(fz_packet_client_leave));
}

void send_client_enter(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_client_enter* packet = reinterpret_cast<fz_packet_client_enter*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_client_enter);
	packet->type = FZ_CLIENT_ENTER;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;

	send_packet_to_server(sid, buf, sizeof(fz_packet_client_enter));
}

void send_proxy_enter(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_proxy_enter* packet = reinterpret_cast<fz_packet_proxy_enter*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_proxy_enter);
	packet->type = FZ_PROXY_ENTER;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;

	send_packet_to_server(sid, buf, sizeof(fz_packet_proxy_enter));
}

void send_proxy_leave(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_proxy_leave* packet = reinterpret_cast<fz_packet_proxy_leave*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_proxy_leave);
	packet->type = FZ_PROXY_LEAVE;

	send_packet_to_server(sid, buf, sizeof(fz_packet_proxy_leave));
}

void send_proxy_move(int sid, int cid)
{
	char* buf = send_buf_pool.get_sendBuffer();

	fz_packet_proxy_move* packet = reinterpret_cast<fz_packet_proxy_move*>(buf);

	packet->id = clients[cid].id;
	packet->size = sizeof(fz_packet_proxy_move);
	packet->type = FZ_PROXY_MOVE;
	packet->x = clients[cid].x;
	packet->y = clients[cid].y;

	send_packet_to_server(sid, buf, sizeof(fz_packet_proxy_move));
}


void DisconnectClient(int id)
{
	clients[id].is_connected = false;
	clients[id].sock.close();
	
	send_disconnect_packet(clients[id].zone_sid, id);
	if (clients[id].in_proxy)
		send_proxy_leave((clients[id].zone_sid + 1) % 2, id);
}

void ProcessLogin(int user_id, char* id_str)
{
	//strcpy_s(clients[user_id]->name, id_str);
	clients[user_id].is_connected = true;

	send_login_ok_packet(user_id);
	send_put_object_packet(user_id, user_id);

	clients[user_id].in_proxy = is_in_boundary(clients[user_id].y);

	send_accept_packet(clients[user_id].zone_sid, user_id);
	if (clients[user_id].in_proxy)
		send_proxy_enter((clients[user_id].zone_sid + 1) % 2, user_id);
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id].x;
	short y = clients[id].y;

	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: std::cout << "Invalid Direction Error\n";
		while (true);
	}

	clients[id].x = x;
	clients[id].y = y;

	if (false == clients[id].in_proxy) {
		send_client_move(clients[id].zone_sid, id);
		if (is_in_boundary(y)) {
			clients[id].in_proxy = true;
			send_proxy_enter((clients[id].zone_sid + 1) % 2, id);
		}
	}
	else {
		if (bounary[clients[id].zone_sid][0] <= y && y <= bounary[clients[id].zone_sid][1]) {
			send_client_move(clients[id].zone_sid, id);
			send_proxy_move((clients[id].zone_sid + 1) % 2, id);
		}
		else if (y < bounary[0][0] || y > bounary[1][1]) {
			clients[id].in_proxy = false;
			send_client_move(clients[id].zone_sid, id);
			send_proxy_leave((clients[id].zone_sid + 1) % 2, id);
		}
		else if ((clients[id].zone_sid == 0 && y > bounary[0][1])
			|| (clients[id].zone_sid == 1 && y < bounary[1][0])) {
			send_client_leave(clients[id].zone_sid, id);
			send_client_enter((clients[id].zone_sid + 1) % 2, id);
			clients[id].zone_sid = (clients[id].zone_sid + 1) % 2;
			send_put_object_packet(id, id);
		}
		else {
			std::cout << "move error" << std::endl;
			while (true);
		}
	}

	send_pos_packet(id, id);
}

void ProcessClientPacket(int id, void* buf)
{
	char* packet = reinterpret_cast<char*>(buf);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		clients[id].move_time = move_packet->move_time;
		ProcessMove(id, move_packet->direction);
	}
				break;
	default: std::cout << "Invalid Packet Type Error\n";
		while (true);
	}
}

void ProcessServerPacket(int sid, void* buf)
{
	char* packet = reinterpret_cast<char*>(buf);
	int* recv_packet_size = reinterpret_cast<int*>(&packet[0]);
	int* to_id = reinterpret_cast<int*>(&packet[sizeof(int)]);

	char* packet_buf = send_buf_pool.get_sendBuffer();

	int send_packet_size = (*recv_packet_size) - sizeof(int) * 2;
	memcpy(packet_buf, &packet[sizeof(int) * 2], send_packet_size);

	send_packet(*to_id, packet_buf, send_packet_size);
}



void accpet_new_client(tcp::socket&& socket, int new_id)
{
	int id = new_id;
	clients[id].id = id;
	clients[id].sock = std::move(socket);
	clients[id].prev_packet_size = 0;
	clients[id].x = fast_rand() % WORLD_WIDTH;
	clients[id].y = fast_rand() % WORLD_HEIGHT;

	if (clients[id].y < WORLD_HEIGHT / 2) clients[id].zone_sid = 0;
	else clients[id].zone_sid = 1;

	clients[id].do_read();

	++new_id;
	client_acceptor.async_accept([new_id](boost::system::error_code ec, tcp::socket socket) {
		accpet_new_client(std::move(socket), new_id);
		});
}

void do_worker()
{
	try {
		io_context.run();
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << "\n";
	}
}

int main()
{
	try {
		// accept to zone servers
		tcp::acceptor zone_acceptor1{ io_context, tcp::endpoint(tcp::v4(), FRONT_ZONE_SERVER_PORT) };
		tcp::socket sock1 = zone_acceptor1.accept();
		zone_servers[0].sock = std::move(sock1);
		zone_servers[0].prev_packet_size = 0;
		zone_servers[0].sid = 0;
		zone_servers[0].do_read();

		tcp::acceptor zone_acceptor2{ io_context, tcp::endpoint(tcp::v4(), FRONT_ZONE_SERVER_PORT + 1) };
		tcp::socket sock2 = zone_acceptor2.accept();
		zone_servers[1].sock = std::move(sock2);
		zone_servers[1].prev_packet_size = 0;
		zone_servers[1].sid = 1;
		zone_servers[1].do_read();

		std::cout << "accept to zone servers" << std::endl;

		std::vector<std::thread> worker_threads;
		for (int i = 0; i < NUM_WORKER_THREADS; ++i)
			worker_threads.emplace_back(do_worker);

		int start_id = 0;
		client_acceptor.async_accept([start_id](boost::system::error_code ec, tcp::socket socket) {
			accpet_new_client(std::move(socket), start_id);
			});

		io_context.run();

		for (auto& t : worker_threads) t.join();
	}
	catch (std::exception e) {
		std::cerr << "Exception: " << e.what() << std::endl;
	}
}
