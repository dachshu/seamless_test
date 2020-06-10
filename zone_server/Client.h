#pragma once
#include "Zone.h"
#include <set>

struct CLIENT
{
	int id;
	int my_woker_id;
	short	x, y;
	unsigned	move_time;
	std::set<int> view_list;

	bool is_connected;

	ZoneNodeBuffer zone_node_buffer;
	std::set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;


	char send_buf[sizeof(int) * 2 + MAX_PACKET_SIZE * MAX_GATHER_SIZE];
	int prev_packet_size;
	int prev_packet_cnt;
};

struct PROXY_CLIENT
{
	int id;
	short	x, y;
	unsigned	move_time;
	std::set<int> view_list;

	bool is_connected;

	ZoneNodeBuffer zone_node_buffer;
	std::set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;
};