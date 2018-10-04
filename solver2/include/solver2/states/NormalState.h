#pragma once
#include "DefaultStateBehavior.h"
#include "../CallsQueueScheduler.h"
#include <csdb/address.h>
#include <vector>

namespace slv2
{
    /// <summary>   A normal node state. This class cannot be inherited. </summary>
    ///
    /// <remarks>   Aae, 30.09.2018. </remarks>
    ///
    /// <seealso cref="T:DefaultStateBehavior"/>

    class NormalState final : public DefaultStateBehavior
    {
    public:

        ~NormalState() override
        {}

        void on(SolverContext& context) override;

        void off(SolverContext& context) override;

        Result onBlock(SolverContext& context, csdb::Pool& block, const PublicKey& sender) override;

        const char * name() const override
        {
            return "Normal";
        }


    private:

        void setup(csdb::Transaction * ptr, SolverContext * pctx);
        int randFT(int min, int max);

        CallsQueueScheduler::CallTag tag_spam { CallsQueueScheduler::no_tag };
        CallsQueueScheduler::CallTag tag_flush { CallsQueueScheduler::no_tag };

        constexpr static uint32_t T_spam_trans = 20;

        constexpr static const int CountSpamKeysVariants = 10;
        std::vector<csdb::Address> spam_keys;
        size_t spam_counter { 0 };
        size_t spam_index { 0 };
    };

} // slv2
