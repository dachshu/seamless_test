#pragma once

#define FRONT_SERVER_PORT		9000
#define FRONT_ZONE_SERVER_PORT	9010

#define MAX_CLIENT 20000
#define WORLD_WIDTH 400
#define WORLD_HEIGHT 400

constexpr int MAX_ID_LEN = 50;

#define CS_LOGIN	1
#define CS_MOVE		2

#define SC_LOGIN_OK			1
//#define SC_LOGIN_FAIL		2
#define SC_POS				3
#define SC_PUT_OBJECT		4
#define SC_REMOVE_OBJECT	5
//#define SC_CHAT				6
//#define SC_STAT_CHANGE		7

#define FZ_ACCEPT		10
#define FZ_DISCONNECT	11
#define FZ_CLIENT_MOVE	12
#define FZ_CLIENT_LEAVE	13
#define FZ_CLIENT_ENTER	14
#define FZ_PROXY_ENTER	15
#define FZ_PROXY_LEAVE	16
#define FZ_PROXY_MOVE	17

#define MAX_PACKET_SIZE 128
#define MAX_GATHER_SIZE 10


#pragma pack(push ,1)

struct sc_packet_login_ok {
	char size;
	char type;
	int id;
	short x, y;
	short hp;
	short level;
	int	exp;
};

struct sc_packet_put_object {
	char size;
	char type;
	int id;
	char o_type;
	short x, y;
	// 렌더링 정보, 종족, 성별, 착용 아이템, 캐릭터 외형, 이름, 길드....
};

struct sc_packet_pos {
	char size;
	char type;
	int id;
	short x, y;
	unsigned move_time;
};

struct cs_packet_login {
	char	size;
	char	type;
	char	id[MAX_ID_LEN];
};

constexpr unsigned char D_UP = 0;
constexpr unsigned char D_DOWN = 1;
constexpr unsigned char D_LEFT = 2;
constexpr unsigned char D_RIGHT = 3;

struct cs_packet_move {
	char	size;
	char	type;
	char	direction;
	unsigned move_time;
};


struct fz_packet_accpet {
	char size;
	char type;
	int id;
	short x, y;
};

struct fz_packet_disconnect {
	char size;
	char type;
	int id;
};

struct fz_packet_client_move {
	char size;
	char type;
	int id;
	short x, y;
	unsigned move_time;
};

struct fz_packet_client_leave {
	char size;
	char type;
	int id;
	short x, y;
};

struct fz_packet_client_enter {
	char size;
	char type;
	int id;
	short x, y;
};

struct fz_packet_proxy_enter {
	char size;
	char type;
	int id;
	short x, y;
};

struct fz_packet_proxy_leave {
	char size;
	char type;
	int id;
};

struct fz_packet_proxy_move {
	char size;
	char type;
	int id;
	short x, y;
};

#pragma pack (pop)
