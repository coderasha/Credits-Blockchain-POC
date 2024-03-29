#ifndef DUMBCV_HPP
#define DUMBCV_HPP

#include <atomic>
#include <map>

#include <lib/system/common.hpp>

namespace cs {
// for answer dumb transactions
class DumbCv {
    const size_t kWaitTimeMs = 30;

public:
    bool addCVInfo(const cs::Signature& signature);
    void sendCvSignal(const cs::Signature& signature);
    bool waitCvSignal(const cs::Signature& signature);

private:
    struct CvInfo {
        std::condition_variable cv;
        std::atomic_bool condFlg{ false };
    };

    std::map<cs::Signature, CvInfo> cvInfo_;
    std::mutex mutex_;
};
}

#endif  // DUMBCV_HPP
