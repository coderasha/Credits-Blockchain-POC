#include "TrustedState.h"
#include "../SolverCore.h"
#include <Solver/Solver.hpp>
#include "../Node.h"
#include <Solver/Generals.hpp>

#include <iostream>

namespace slv2
{
    void TrustedState::on(SolverContext& context)
    {
        // its possible vectors or matrices already completed
        if(test_vectors_completed(context)) {
            // let context decide what to do
            context.vectors_completed();
        }
        if(test_matrices_completed(context)) {
            // let context decide what to do
            context.matrices_completed();
        }
        is_block_recv = false;
    }

    Result TrustedState::onRoundTable(SolverContext& /*context*/, const uint32_t round)
    {
        std::cout << name() << ": round table received: " << round << std::endl;
        return Result::Finish;
    }

    Result TrustedState::onVector(SolverContext& context, const Credits::HashVector & vect, const PublicKey & /*sender*/)
    {
        if(context.is_vect_recv_from(vect.Sender)) {
            //std::cout << "SOLVER> I've already got the vector from this Node" << std::endl;
            return Result::Ignore;
        }
        context.recv_vect_from(vect.Sender);
        context.generals().addvector(vect); // building matrix

        if(test_vectors_completed(context))
        {
            //compose and send matrix!!!
            auto my_num = context.conf_number();
            context.generals().addSenderToMatrix(my_num);

            // context.generals().addmatrix(context.generals().getMatrix(), context.node().getConfidants()); is called from next:
            onMatrix(context, context.generals().getMatrix(), PublicKey {});

            context.node().sendMatrix(context.generals().getMatrix());
            return Result::Finish;

        }
        std::cout << name() << ": vector received" << std::endl;
        return Result::Ignore;
    }

    Result TrustedState::onMatrix(SolverContext& context, const Credits::HashMatrix & matr, const PublicKey & /*sender*/)
    {
        if(is_block_recv) {
            // this logic is from original solver-1
            return Result::Ignore;
        }

        if(context.is_matr_recv_from(matr.Sender)) {
            //std::cout << "SOLVER> I've already got the matrix from this Node" << std::endl;
            return Result::Ignore;
        }
        context.recv_matr_from(matr.Sender);
        context.generals().addmatrix(matr, context.node().getConfidants());

        if(test_matrices_completed(context)) {
            return Result::Finish;
        }
        return Result::Ignore;
    }

    Result TrustedState::onBlock(SolverContext & /*context*/, const csdb::Pool& /*pool*/, const PublicKey & /*sender*/)
    {
        // to be tested in onMatrix(), logic from original solver-1
        is_block_recv = true;
        //TODO: to be implemented
        return Result::Ignore;
    }

    bool TrustedState::test_vectors_completed(const SolverContext& context) const
    {
        return context.cnt_vect_recv() == context.node().getConfidants().size();
    }
    bool TrustedState::test_matrices_completed(const SolverContext& context) const
    {
        return context.cnt_matr_recv() == context.node().getConfidants().size();
    }

} // slv2
