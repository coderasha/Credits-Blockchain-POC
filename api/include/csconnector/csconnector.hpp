#ifndef CSCONNCETOR_HPP
#define CSCONNCETOR_HPP

#if defined(_MSC_VER)
#pragma warning(push, 0) // 4245: 'return': conversion from 'int' to 'SOCKET', signed/unsigned mismatch
#endif

#include <apihandler.hpp>

#include <thrift/server/TThreadPoolServer.h>
#include <thrift/server/TThreadedServer.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <client/params.hpp>

#include <csdb/pool.hpp>

#include <solvercore.hpp>

#include <memory>
#include <thread>

#ifdef PROFILE_API
#include <profiler/profilerprocessor.hpp>
#include <profiler/profilereventhandler.hpp>
#endif

namespace csconnector {
class connector {
public:
    using ApiHandlerPtr = ::apache::thrift::stdcxx::shared_ptr<api::APIHandler>;
    using ApiExecHandlerPtr = ::apache::thrift::stdcxx::shared_ptr<apiexec::APIEXECHandler>;

#ifdef PROFILE_API
    using ApiProcessor = cs::ProfilerProcessor;
#else
    using ApiProcessor = ::api::APIProcessor;
#endif

    explicit connector(BlockChain& m_blockchain, cs::SolverCore* solver);
    ~connector();

    connector(const connector&) = delete;
    connector& operator=(const connector&) = delete;

    void onReadFromDB(csdb::Pool pool, bool* should_stop) {
        if (!*should_stop) {
            api_handler->updateSmartCachesPool(pool);
#ifdef MONITOR_NODE
            api_handler->collect_all_stats_slot(pool);
#endif
            api_handler->baseLoaded(pool);
        }
    }

    void onStoreBlock(const csdb::Pool& pool) {
        api_handler->store_block_slot(pool);
    }

    void onMaxBlocksCount(cs::Sequence lastBlockNum) {
        api_handler->maxBlocksCount(lastBlockNum);
    }

    void run();

    // interface
    ApiHandlerPtr apiHandler() const;
    ApiExecHandlerPtr apiExecHandler() const;

private:
    cs::Executor& executor_;
    ApiHandlerPtr api_handler;
    ApiExecHandlerPtr apiexec_handler;

    ::apache::thrift::stdcxx::shared_ptr<ApiProcessor> p_api_processor;
    ::apache::thrift::stdcxx::shared_ptr<::apiexec::APIEXECProcessor> p_apiexec_processor;
#ifdef BINARY_TCP_API
    ::apache::thrift::server::TThreadedServer server;
    std::thread thread;
    uint16_t server_port;
#endif
#ifdef AJAX_IFACE
    ::apache::thrift::server::TThreadedServer ajax_server;
    std::thread ajax_thread;
    uint16_t ajax_server_port;
#endif
#ifdef BINARY_TCP_EXECAPI
    ::apache::thrift::server::TThreadedServer exec_server;
    std::thread exec_thread;
    uint16_t exec_server_port;
#endif
};
}  // namespace csconnector

#endif  // CSCONNCETOR_HPP
