#pragma once

//#define MONITOR_NODE
//#define SPAM_MAIN
//#define STARTER
//#define NO_CONSENSUS
//#define TOKENS_CACHE
#define AJAX_IFACE
#define CUSTOMER_NODE
//#define FOREVER_ALONE
#define TIME_TO_COLLECT_TRXNS 50
#define TIME_TO_AWAIT_ACTIVITY 200
#define TRX_SLEEP_TIME 50000  // microseconds
#define FAKE_BLOCKS
#ifndef MONITOR_NODE
//#define SPAMMER
#else
#define STATS
#endif
#define SYNCRO
#define MYLOG
//#define LOG_TRANSACTIONS

#define BOTTLENECKED_SMARTS
#define AJAX_CONCURRENT_API_CLIENTS INT64_MAX
#define BINARY_TCP_API
#define DEFAULT_CURRENCY 1

constexpr auto   SIZE_OF_COMMON_TRANSACTION  = 190;
constexpr double COST_OF_ONE_TRUSTED_PER_DAY = 17;
