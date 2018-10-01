#pragma once
#include "DefaultStateBehavior.h"
#include "Solver/CallsQueueScheduler.h"

#if defined(SOLVER_USES_PROXY_TYPES)
#include "ProxyTypes.h"
#else
#include <csdb/pool.h>
#endif

namespace slv2
{
    /// <summary>   A transaction collector node state (so called "main node"). This class cannot be inherited. </summary>
    ///
    /// <remarks>   Aae, 30.09.2018. </remarks>
    ///
    /// <seealso cref="T:DefaultStateBehavior"/>

    class CollectState final : public DefaultStateBehavior
    {
    public:

        ~CollectState() override
        {}

        void on(SolverContext& context) override;

        /**
         * @fn  virtual void final::onRoundEnd(SolverContext& context) override;
         *
         * @brief   Cancel round timeout if set. Sends list of transactions collected this round
         *
         * @author  aae
         * @date    01.10.2018
         *
         * @param [in,out]  context The context.
         */

        virtual void onRoundEnd(SolverContext& context) override;

        Result onTransaction(SolverContext& context, const csdb::Transaction& tr) override;

        Result onTransactionList(SolverContext& context, const csdb::Pool& pool) override;

        const char * name() const override
        {
            return "Collect";
        }

    private:

        size_t cnt_transactions { 0 };
        csdb::Pool pool;
        CallsQueueScheduler::CallTag tag_timeout;
    };

} // slv2
