#pragma once

///////windows
#include <boost/asio.hpp>


using boost::asio::ip::tcp;

#define NUM_WORKER_THREADS	4
#define NUM_TOTAL_WORKERS	(NUM_WORKER_THREADS + 2)
#define MAX_DQ_REUSLTS		int( MAX_CLIENT )
#define LOCAL_SEND_BUF_CNT	int((MAX_CLIENT * 128))
#define INIT_NUM_ZONE_NODE		16
#define ZONE_SIZE			20
constexpr auto VIEW_RANGE = 7;

thread_local int tid;
