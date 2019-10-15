/* Send blaming letters to @yrtimd */
#include "transport.hpp"
#include "network.hpp"

#include <csnode/node.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/packstream.hpp>

#include <lib/system/allocators.hpp>
#include <lib/system/utils.hpp>

#include <thread>

// Signal transport to stop and stop Node
static void stopNode() noexcept(false) {
    Node::requestStop();
    // Transport::stop();
}

// Called periodically to poll the signal flag.
void pollSignalFlag() {
    if (gSignalStatus == 1) {
        gSignalStatus = 0;
        try {
            stopNode();
        }
        catch (...) {
            cserror() << "Poll signal error!";
            std::raise(SIGABRT);
        }
    }
}

// Extern function dfined in main.cpp to poll and handle signal status.
extern void pollSignalFlag();

enum RegFlags : uint8_t {
    UsingIPv6 = 1,
    RedirectIP = 1 << 1,
    RedirectPort = 1 << 2
};

enum Platform : uint8_t {
    Linux,
    MacOS,
    Windows
};

Transport::Transport(const Config& config, Node* node)
: config_(config)
, sendPacksFlag_()
, remoteNodes_(maxRemoteNodes_ + 1)
, myPublicKey_(node->getNodeIdKey())
, oLock_()
, oPackStream_(&netPacksAllocator_, node->getNodeIdKey())
, uLock_()
, net_(new Network(config, this))
, node_(node)
, nh_(this) {
    good_ = net_->isGood();
}

Transport::~Transport() {
    delete net_;
}

void Transport::run() {
    net_->sendInit();
    acceptRegistrations_ = config_->getNodeType() == NodeType::Router;

    {
        cs::Lock lock(oLock_);
        oPackStream_.init(BaseFlags::NetworkMsg);
        formRegPack(&regPackConnId_, myPublicKey_, node_->getBlockChain().uuid());
        regPack_ = *(oPackStream_.getPackets());
        oPackStream_.clear();
    }

    refillNeighbourhood();

    // Okay, now let's get to business
    uint32_t ctr = 0;
    cswarning() << "+++++++>>> Transport Run Task Start <<<+++++++++++++++";

    // Check if thread is requested to stop ?
    while (Transport::gSignalStatus == 0) {
        ++ctr;

        bool askMissing = false;
        bool resendPacks = ctr % 11 == 0;
        bool sendPing = ctr % 19 == 0;
        bool refreshLimits = ctr % 23 == 0;
        bool checkPending = ctr % 101 == 0;
        bool checkSilent = ctr % 151 == 0;

        if (askMissing) {
            askForMissingPackages();
        }

        if (checkPending) {
            nh_.checkPending(config_->getMaxNeighbours());
        }

        if (checkSilent) {
            nh_.checkSilent();
            nh_.checkNeighbours();
        }

        if (resendPacks) {
            nh_.resendPackets();
        }

        if (sendPing) {
            nh_.pingNeighbours();
        }

        if (refreshLimits) {
            nh_.refreshLimits();
        }

        pollSignalFlag();
        emit mainThreadIterated();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    cswarning() << "[Transport::run STOPED!]";
}

const char* Transport::networkCommandToString(NetworkCommand command) {
    switch (command) {
    case NetworkCommand::Registration:
        return "Registration";
    case NetworkCommand::ConfirmationRequest:
        return "ConfirmationRequest";
    case NetworkCommand::ConfirmationResponse:
        return "ConfirmationResponse";
    case NetworkCommand::RegistrationConfirmed:
        return "RegistrationConfirmed";
    case NetworkCommand::RegistrationRefused:
        return "RegistrationRefused";
    case NetworkCommand::Ping:
        return "Ping";
    case NetworkCommand::PackInform:
        return "PackInform";
    case NetworkCommand::PackRequest:
        return "PackRequest";
    case NetworkCommand::PackRenounce:
        return "PackRenounce";
    case NetworkCommand::BlockSyncRequest:
        return "BlockSyncRequest";
    case NetworkCommand::SSRegistration:
        return "SSRegistration";
    case NetworkCommand::SSFirstRound:
        return "SSFirstRound";
    case NetworkCommand::SSRegistrationRefused:
        return "SSRegistrationRefused";
    case NetworkCommand::SSPingWhiteNode:
        return "SSPingWhiteNode";
    case NetworkCommand::SSLastBlock:
        return "SSLastBlock";
    case NetworkCommand::SSReRegistration:
        return "SSReRegistration";
    case NetworkCommand::SSSpecificBlock:
        return "SSSpecificBlock";
    default:
        return "Unknown";
    }
}

template <>
uint16_t getHashIndex(const ip::udp::endpoint& ep) {
    uint16_t result = ep.port();

    if (ep.protocol() == ip::udp::v4()) {
        uint32_t address = ep.address().to_v4().to_uint();
        uint16_t lowBits = static_cast<uint16_t>(address);
        uint16_t highBits = address >> (sizeof(uint16_t) * CHAR_BIT);
        result ^= lowBits ^ highBits;
    }
    else {
        auto bytes = ep.address().to_v6().to_bytes();
        auto ptr = reinterpret_cast<uint8_t*>(&result);
        auto bytesPtr = bytes.data();
        for (size_t i = 0; i < 8; ++i) {
            *ptr ^= *(bytesPtr++);
        }

        ++ptr;

        for (size_t i = 8; i < 16; ++i) {
            *ptr ^= *(bytesPtr++);
        }
    }

    return result;
}

RemoteNodePtr Transport::getPackSenderEntry(const ip::udp::endpoint& ep) {
    auto& rn = remoteNodesMap_.tryStore(ep);

    if (!rn) {  // Newcomer
        rn = remoteNodes_.emplace();
    }

    rn->packets.fetch_add(1, std::memory_order_relaxed);
    return rn;
}

bool Transport::sendDirect(const Packet* pack, const Connection& conn) {
    conn.lastBytesCount.fetch_add(static_cast<uint32_t>(pack->size()), std::memory_order_relaxed);
    net_->sendDirect(*pack, conn.getOut());
    return true;
}

bool Transport::sendDirectToSock(Packet* pack, const Connection& conn) {
    conn.lastBytesCount.fetch_add(static_cast<uint32_t>(pack->size()), std::memory_order_relaxed);
    net_->sendPackDirect(*pack, conn.getOut());
    return true;
}

void Transport::deliverDirect(const Packet* pack, const uint32_t size, ConnectionPtr conn) {
    if (size >= Packet::MaxFragments) {
        ++Transport::cntExtraLargeNotSent;
        csinfo() << __func__ << ": packSize(" << Transport::cntExtraLargeNotSent << ") = " << size;
    }

    if (size > 1000) {
        std::vector<Packet> _packets(size);
        for (auto& p : _packets) p = *pack++;
        std::thread thread([=, packets = std::move(_packets)]() mutable {
            uint32_t allSize = size, toSend, j = 0;

            while(allSize != 0) {
                if (allSize > 100) {
                    toSend = 100;
                    allSize -= 100;
                }
                else {
                    toSend = allSize;
                    allSize = 0;
                }

                {
                    for (uint32_t i = 0; i < toSend; i++) {
                        nh_.registerDirect(&packets[j], conn);
                        sendDirectToSock(&packets[j++], **conn);
                    }
                }

                std::this_thread::yield();
            }
        });
        thread.detach();
    } else {
        const auto packEnd = pack + size;
        for (auto ptr = pack; ptr != packEnd; ++ptr) {
            nh_.registerDirect(ptr, conn);
            sendDirect(ptr, **conn);
        }
    }
}

void Transport::deliverBroadcast(const Packet* pack, const uint32_t size) {
    if (size >= Packet::MaxFragments) {
        ++Transport::cntExtraLargeNotSent;
        csinfo() << __func__ << ": packSize(" << Transport::cntExtraLargeNotSent << ") = " << size;
    }

    if (size > 1000) {
        std::vector<Packet> _packets(size);
        for (auto& p : _packets) p = *pack++;
        std::thread thread([=, packets = std::move(_packets)]() {
            {
                auto lock = getNeighboursLock();
                sendLarge_.store(true, std::memory_order_relaxed);
                nh_.chooseNeighbours();
            }

            uint32_t allSize = size, toSend, j = 0;

            while(allSize != 0) {
                if (allSize > 100) {
                    toSend = 100;
                    allSize -= 100;
                }
                else {
                    toSend = allSize;
                    allSize = 0;
                }

                {
                    auto lock = getNeighboursLock();
                    for (uint32_t i = 0; i < toSend; i++) {
                        nh_.sendByNeighbours(&packets[j++], true);
                    }
                }

                std::this_thread::yield();
            }

            sendLarge_.store(false, std::memory_order_release);
        });
        thread.detach();
    }
    else {
        auto lock = getNeighboursLock();
        nh_.chooseNeighbours();

        const auto packEnd = pack + size;
        for (auto ptr = pack; ptr != packEnd; ++ptr) {
            sendBroadcast(ptr);
        }
    }
}

void Transport::deliverConfidants(const Packet* pack, const uint32_t size) {
    if (!nh_.isConfidants()) {
        deliverBroadcast(pack, size);
        return;
    }
    if (size >= Packet::MaxFragments) {
        ++Transport::cntExtraLargeNotSent;
        csinfo() << __func__ << ": packSize(" << Transport::cntExtraLargeNotSent << ") = " << size;
    }

    const auto packEnd = pack + size;
    for (auto ptr = pack; ptr != packEnd; ++ptr) {
        nh_.sendByConfidants(ptr);
    }
}

// Processing network packages

void Transport::processNetworkTask(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
    iPackStream_.init(task->pack.getMsgData(), task->pack.getMsgSize());

    NetworkCommand cmd;
    iPackStream_ >> cmd;

    if (!iPackStream_.good()) {
        return sender->addStrike();
    }

    if (cmd != NetworkCommand::Registration) {
        if (sender->isBlackListed()) {
            csdebug() << "Network command is ignored from blacklisted " << task->sender;
            return;
        }
    }

    bool result = true;
    switch (cmd) {
        case NetworkCommand::Registration:
            result = gotRegistrationRequest(task, sender);
            break;
        case NetworkCommand::ConfirmationRequest:
            break;
        case NetworkCommand::ConfirmationResponse:
            break;
        case NetworkCommand::RegistrationConfirmed:
            result = gotRegistrationConfirmation(task, sender);
            break;
        case NetworkCommand::RegistrationRefused:
            result = gotRegistrationRefusal(task, sender);
            break;
        case NetworkCommand::Ping:
            gotPing(task, sender);
            break;
        case NetworkCommand::SSRegistration:
            gotSSRegistration(task, sender);
            break;
        case NetworkCommand::SSReRegistration:
            gotSSReRegistration();
            break;
        case NetworkCommand::SSFirstRound:
            gotSSDispatch(task);
            break;
        case NetworkCommand::SSRegistrationRefused:
            gotSSRefusal(task);
            break;
        case NetworkCommand::SSPingWhiteNode:
            gotSSPingWhiteNode(task);
            break;
        case NetworkCommand::SSLastBlock: {
            long long timeSS{};
            iPackStream_ >> timeSS;
            node_->setDeltaTimeSS(timeSS);

            gotSSLastBlock(task, node_->getBlockChain().getLastSeq(), node_->getBlockChain().getLastHash(),
                node_->canBeTrusted(true /*crirical, all trusted required*/));
            break;
        }
        case NetworkCommand::SSSpecificBlock: {
            cs::RoundNumber round = 0;
            iPackStream_ >> round;

            long long timeSS{};
            iPackStream_ >> timeSS;
            node_->setDeltaTimeSS(timeSS);

            if (node_->getBlockChain().getLastSeq() < round) {
                gotSSLastBlock(task, node_->getBlockChain().getLastSeq(), node_->getBlockChain().getLastHash(), false);
            }
            else {
                gotSSLastBlock(task, round, node_->getBlockChain().getHashBySequence(round),
                    node_->canBeTrusted(true /*crirical, all trusted required*/));
            }
            break;
        }
        case NetworkCommand::SSNewFriends:
            gotSSNewFriends();
            break;
        case NetworkCommand::SSUpdateServer:
            gotSSUpdateServer();
            break;
        case NetworkCommand::PackInform:
            gotPackInform(task, sender);
            break;
        case NetworkCommand::PackRenounce:
            // gotPackRenounce(task, sender);
            break;
        case NetworkCommand::PackRequest:
            // gotPackRequest(task, sender);
            break;
        case NetworkCommand::IntroduceConsensusReply:
            gotSSIntroduceConsensusReply();
            break;
        default:
            result = false;
            cswarning() << "Unexpected network command";
    }

    if (!result) {
        sender->addStrike();
    }
}

void Transport::refillNeighbourhood() {
    // TODO: check this algorithm when all list nodes are dead
    if (config_->getBootstrapType() == BootstrapType::IpList) {
        for (auto& ep : config_->getIpList()) {
            if (!nh_.canHaveNewConnection()) {
                cswarning() << "Connections limit reached";
                break;
            }

            cslog() << "Creating connection to " << ep.ip;
            nh_.establishConnection(net_->resolve(ep));
        }
    }

    if (config_->getBootstrapType() == BootstrapType::SignalServer || config_->getNodeType() == NodeType::Router) {
        // Connect to SS logic
        ssEp_ = net_->resolve(config_->getSignalServerEndpoint());
        cslog() << "Connecting to start node on " << ssEp_;

        {
            cs::Lock lock(oLock_);
            formSSConnectPack(myPublicKey_, node_->getBlockChain().uuid());
            ssStatus_ = SSBootstrapStatus::Requested;
            net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
        }
    }
}

bool Transport::parseSSSignal(const TaskPtr<IPacMan>& task) {
    iPackStream_.init(task->pack.getMsgData(), task->pack.getMsgSize());
    iPackStream_.safeSkip<uint8_t>(1);
    iPackStream_.safeSkip<cscrypto::PublicKey>(1);

    cs::RoundNumber rNum = 0;
    iPackStream_ >> rNum;

    auto trStart = iPackStream_.getCurrentPtr();

    uint8_t numConf;
    iPackStream_ >> numConf;

    if (!iPackStream_.good()) {
        return false;
    }

    iPackStream_.safeSkip<cs::PublicKey>(numConf + 1);

    auto trFinish = iPackStream_.getCurrentPtr();
    node_->getRoundTableSS(trStart, cs::numeric_cast<size_t>(trFinish - trStart), rNum);

    uint8_t numCirc;
    iPackStream_ >> numCirc;

    if (!iPackStream_.good()) {
        return false;
    }

    uint32_t ctr = nh_.size();

    if (config_->getBootstrapType() == BootstrapType::SignalServer) {
        for (uint8_t i = 0; i < numCirc; ++i) {
            EndpointData ep;
            ep.ipSpecified = true;
            cs::PublicKey key;

            iPackStream_ >> ep.ip >> ep.port >> key;

            if (!iPackStream_.good()) {
                return false;
            }

            ++ctr;

            if (!std::equal(key.cbegin(), key.cend(), config_->getMyPublicKey().cbegin())) {
                if (ctr <= config_->getMaxNeighbours()) {
                    nh_.establishConnection(net_->resolve(ep));
                }
            }

            if (!nh_.canHaveNewConnection()) {
                break;
            }
        }
    }

    ssStatus_ = SSBootstrapStatus::Complete;
    return true;
}

constexpr const uint32_t StrippedDataSize = sizeof(cs::RoundNumber) + sizeof(MsgTypes);
void Transport::processNodeMessage(const Message& msg) {
    auto type = msg.getFirstPack().getType();
    auto rNum = msg.getFirstPack().getRoundNum();

    switch (node_->chooseMessageAction(rNum, type, msg.getFirstPack().getSender())) {
        case Node::MessageActions::Process:
            return dispatchNodeMessage(type, rNum, msg.getFirstPack(), msg.getFullData() + StrippedDataSize, msg.getFullSize() - StrippedDataSize);
        case Node::MessageActions::Postpone:
            return postponePacket(rNum, type, msg.extractData());
        case Node::MessageActions::Drop:
            return;
    }
}

constexpr cs::RoundNumber getRoundTimeout(const MsgTypes type) {
    switch (type) {
        case MsgTypes::FirstSmartStage:
        case MsgTypes::SecondSmartStage:
        case MsgTypes::ThirdSmartStage:
        case MsgTypes::RejectedContracts:
            return 100;
        case MsgTypes::TransactionPacket:
        case MsgTypes::TransactionsPacketRequest:
        case MsgTypes::TransactionsPacketReply:
            return cs::Conveyer::MetaCapacity;
        default:
            return 5;
    }
}

bool Transport::shouldSendPacket(const Packet& pack) {
    if (pack.isNetwork()) {
        return false;
    }

    const cs::RoundNumber currentRound = cs::Conveyer::instance().currentRoundNumber();

    if (!pack.isFragmented()) {
        return (pack.getRoundNum() + getRoundTimeout(pack.getType())) >= currentRound;
    }

    auto& rn = fragOnRound_.tryStore(pack.getHeaderHash());

    if (pack.getFragmentId() == 0) {
        rn = pack.getRoundNum() + getRoundTimeout(pack.getType());
    }

    return !rn || rn >= currentRound;
}

void Transport::processNodeMessage(const Packet& pack) {
    auto type = pack.getType();
    auto rNum = pack.getRoundNum();

    switch (node_->chooseMessageAction(rNum, type, pack.getSender())) {
        case Node::MessageActions::Process:
            return dispatchNodeMessage(type, rNum, pack, pack.getMsgData() + StrippedDataSize, pack.getMsgSize() - StrippedDataSize);
        case Node::MessageActions::Postpone:
            return postponePacket(rNum, type, pack);
        case Node::MessageActions::Drop:
            return;
    }
}

inline void Transport::postponePacket(const cs::RoundNumber rNum, const MsgTypes type, const Packet& pack) {
    (*postponed_)->emplace(rNum, type, pack);
}

void Transport::processPostponed(const cs::RoundNumber rNum) {
    auto& ppBuf = *postponed_[1];
    for (auto& pp : **postponed_) {
        if (pp.round > rNum) {
            ppBuf.emplace(std::move(pp));
        }
        else if (pp.round == rNum) {
            dispatchNodeMessage(pp.type, pp.round, pp.pack, pp.pack.getMsgData() + StrippedDataSize, pp.pack.getMsgSize() - StrippedDataSize);
        }
    }

    (*postponed_)->clear();

    postponed_[1] = *postponed_;
    postponed_[0] = &ppBuf;

    csdebug() << "TRANSPORT> POSTPHONED finished, round " << rNum;
}

void Transport::dispatchNodeMessage(const MsgTypes type, const cs::RoundNumber rNum, const Packet& firstPack, const uint8_t* data, size_t size) {
    if (size == 0) {
        cserror() << "Bad packet size, why is it zero?";
        return;
    }

    // cut my packs
    if (firstPack.getSender() == node_->getNodeIdKey()) {
        csdebug() << "TRANSPORT> Ignore own packs";
        return;
    }

    // never cut packets
    switch (type) {
        case MsgTypes::BlockRequest:
            return node_->getBlockRequest(data, size, firstPack.getSender());
        case MsgTypes::RequestedBlock:
            return node_->getBlockReply(data, size);
        case MsgTypes::BigBang:  // any round (in theory) may be set
            return node_->getBigBang(data, size, rNum);
        case MsgTypes::RoundTableRequest:  // old-round node may ask for round info
            return node_->getRoundTableRequest(data, size, rNum, firstPack.getSender());
        case MsgTypes::NodeStopRequest:
            return node_->getNodeStopRequest(rNum, data, size);
        case MsgTypes::RoundTable:
            return node_->getRoundTable(data, size, rNum, firstPack.getSender());
        case MsgTypes::RoundTableSS:
            return node_->getRoundTableSS(data, size, rNum);
        default:
            break;
    }

    // cut slow packs
    if ((rNum + getRoundTimeout(type)) < cs::Conveyer::instance().currentRoundNumber()) {
        csdebug() << "TRANSPORT> Ignore old packs, round " << rNum << ", type " << Packet::messageTypeToString(type) << ", fragments " << firstPack.getFragmentsNum();
        return;
    }

    if (type == MsgTypes::ThirdSmartStage) {
        csdebug() << "+++++++++++++++++++  ThirdSmartStage arrived +++++++++++++++++++++";
    }

    // packets which transport may cut
    switch (type) {
        case MsgTypes::BlockHash:
            return node_->getHash(data, size, rNum, firstPack.getSender());
        case MsgTypes::HashReply:
            return node_->getHashReply(data, size, rNum, firstPack.getSender());
        case MsgTypes::TransactionPacket:
            return node_->getTransactionsPacket(data, size);
        case MsgTypes::TransactionsPacketRequest:
            return node_->getPacketHashesRequest(data, size, rNum, firstPack.getSender());
        case MsgTypes::TransactionsPacketReply:
            return node_->getPacketHashesReply(data, size, rNum, firstPack.getSender());
        case MsgTypes::FirstStage:
            return node_->getStageOne(data, size, firstPack.getSender());
        case MsgTypes::SecondStage:
            return node_->getStageTwo(data, size, firstPack.getSender());
        case MsgTypes::FirstStageRequest:
            return node_->getStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::SecondStageRequest:
            return node_->getStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::ThirdStageRequest:
            return node_->getStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::ThirdStage:
            return node_->getStageThree(data, size);
        case MsgTypes::FirstSmartStage:
            return node_->getSmartStageOne(data, size, rNum, firstPack.getSender());
        case MsgTypes::SecondSmartStage:
            return node_->getSmartStageTwo(data, size, rNum, firstPack.getSender());
        case MsgTypes::ThirdSmartStage:
            return node_->getSmartStageThree(data, size, rNum, firstPack.getSender());
        case MsgTypes::SmartFirstStageRequest:
            return node_->getSmartStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::SmartSecondStageRequest:
            return node_->getSmartStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::SmartThirdStageRequest:
            return node_->getSmartStageRequest(type, data, size, firstPack.getSender());
        case MsgTypes::RejectedContracts:
            return node_->getSmartReject(data, size, rNum, firstPack.getSender());
        case MsgTypes::RoundTableReply:
            return node_->getRoundTableReply(data, size, firstPack.getSender());
        case MsgTypes::RoundPackRequest:
            return node_->getRoundPackRequest(data, size, rNum, firstPack.getSender());
        case MsgTypes::EmptyRoundPack:
            return node_->getEmptyRoundPack(data, size, rNum, firstPack.getSender());
        case MsgTypes::StateRequest:
            return node_->getStateRequest(data, size, rNum, firstPack.getSender());
        case MsgTypes::StateReply:
            return node_->getStateReply(data, size, rNum, firstPack.getSender());
        default:
            cserror() << "TRANSPORT> Unknown message type " << Packet::messageTypeToString(type) << " pack round " << rNum;
            break;
    }
}

void Transport::registerTask(Packet* pack, const uint32_t packNum, const bool incrementWhenResend) {
    auto end = pack + packNum;

    for (auto ptr = pack; ptr != end; ++ptr) {
        cs::Lock lock(sendPacksFlag_);
        PackSendTask pst;
        pst.pack = *ptr;
        pst.incrementId = incrementWhenResend;
        sendPacks_.emplace(pst);
    }
}

uint32_t Transport::getNeighboursCount() {
    return nh_.size();
}

uint32_t Transport::getNeighboursCountWithoutSS() {
    return nh_.getNeighboursCountWithoutSS();
}

uint32_t Transport::getMaxNeighbours() const {
    return config_->getMaxNeighbours();
}

ConnectionPtr Transport::getConnectionByKey(const cs::PublicKey& pk) {
    return nh_.getNeighbourByKey(pk);
}

ConnectionPtr Transport::getConnectionByNumber(const std::size_t number) {
    return nh_.getNeighbour(number);
}

cs::Sequence Transport::getConnectionLastSequence(const std::size_t number) {
    ConnectionPtr ptr = getConnectionByNumber(number);
    if (ptr && !ptr->isSignal) {
        return ptr->lastSeq;
    }
    return cs::Sequence{};
}

ConnectionPtr Transport::getRandomNeighbour() {
    csdebug() << "Transport> Get random neighbour for Sync";
    return nh_.getRandomSyncNeighbour();
}


void Transport::forEachNeighbour(std::function<void(ConnectionPtr)> func) {
    nh_.forEachNeighbour(std::move(func));
}

void Transport::forEachNeighbourWithoudSS(std::function<void(ConnectionPtr)> func) {
    nh_.forEachNeighbourWithoutSS(std::move(func));
}

bool Transport::forRandomNeighbour(std::function<void(ConnectionPtr)> func) {
    return nh_.forRandomNeighbour(std::move(func));
}

const Connections Transport::getNeighbours() const {
    return nh_.getNeigbours();
}

const Connections Transport::getNeighboursWithoutSS() const {
    return nh_.getNeighboursWithoutSS();
}

bool Transport::isPingDone() {
    return nh_.isPingDone();
}

void Transport::resetNeighbours() {
    csdebug() << "Transport> Reset neighbours";
    return nh_.resetSyncNeighbours();
}

void Transport::onConfigChanged(const Config& updated) {
    config_.exchange(updated);
}

void Transport::addMyOut(const uint8_t initFlagValue) {
    uint8_t regFlag = 0;
    if (!config_->isSymmetric()) {
        if (config_->getAddressEndpoint().ipSpecified) {
            regFlag |= RegFlags::RedirectIP;
            if (config_->getAddressEndpoint().ip.is_v6()) {
                regFlag |= RegFlags::UsingIPv6;
            }
        }

        regFlag |= RegFlags::RedirectPort;
    }
    else if (config_->hasTwoSockets()) {
        regFlag |= RegFlags::RedirectPort;
    }

    uint8_t* flagChar = oPackStream_.getCurrentPtr();

    if (!config_->isSymmetric()) {
        if (config_->getAddressEndpoint().ipSpecified) {
            oPackStream_ << config_->getAddressEndpoint().ip;
        }
        else {
            uint8_t c = 0_b;
            oPackStream_ << c;
        }

        oPackStream_ << config_->getAddressEndpoint().port;
    }
    else if (config_->hasTwoSockets()) {
        oPackStream_ << 0_b << config_->getInputEndpoint().port;
    }
    else {
        oPackStream_ << 0_b;
    }

    *flagChar |= initFlagValue | regFlag;
}

void Transport::formRegPack(uint64_t** regPackConnId, const cs::PublicKey& pk, uint64_t uuid) {
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::Registration << NODE_VERSION << uuid;

    addMyOut();
    *regPackConnId = reinterpret_cast<uint64_t*>(oPackStream_.getCurrentPtr());

    oPackStream_ << static_cast<ConnectionId>(0) << pk;
}

void Transport::formSSConnectPack(const cs::PublicKey& pk, uint64_t uuid) {
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::SSRegistration
#ifdef _WIN32
        << Platform::Windows
#elif __APPLE__
        << Platform::MacOS
#else
        << Platform::Linux
#endif
        << NODE_VERSION << uuid;

    uint8_t flag = (config_->getNodeType() == NodeType::Router) ? 8 : 0;
    addMyOut(flag);

    oPackStream_ << pk;
}

/* Sending network tasks */
void Transport::sendRegistrationRequest(Connection& conn) {
    RemoteNodePtr ptr = getPackSenderEntry(conn.getOut());

    if (ptr->isBlackListed()) {
        return;
    }

    cslog() << "Sending registration request to " << (conn.specialOut ? conn.out : conn.in);

    cs::Lock lock(oLock_);
    Packet req(netPacksAllocator_.allocateNext(cs::numeric_cast<uint32_t>(regPack_.size())));
    *regPackConnId_ = conn.id;
    memcpy(req.data(), regPack_.data(), regPack_.size());

    ++(conn.attempts);
    sendDirect(&req, conn);
}

void Transport::sendRegistrationConfirmation(const Connection& conn, const Connection::Id requestedId) {
    cslog() << "Confirming registration with " << conn.getOut();

    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::RegistrationConfirmed << requestedId << conn.id << myPublicKey_;

    sendDirect(oPackStream_.getPackets(), conn);
    oPackStream_.clear();
}

void Transport::sendRegistrationRefusal(const Connection& conn, const RegistrationRefuseReasons reason) {
    cslog() << "Refusing registration with " << conn.in;

    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::RegistrationRefused << conn.id << reason;

    sendDirect(oPackStream_.getPackets(), conn);
    oPackStream_.clear();
}

// Requests processing
bool Transport::gotRegistrationRequest(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
    cslog() << "Got registration request from " << task->sender;

    NodeVersion vers;
    uint64_t remote_uuid = 0;
    iPackStream_ >> vers >> remote_uuid;

    if (!iPackStream_.good()) {
        return false;
    }

    Connection conn;
    conn.in = task->sender;
    auto& flags = iPackStream_.peek<uint8_t>();

    if (flags & RegFlags::RedirectIP) {
        boost::asio::ip::address addr;
        iPackStream_ >> addr;

        conn.out.address(addr);
        conn.specialOut = true;
    }
    else {
        conn.specialOut = false;
        iPackStream_.skip<uint8_t>();
    }

    if (flags & RegFlags::RedirectPort) {
        Port port = Port();
        iPackStream_ >> port;

        if (!conn.specialOut) {
            conn.specialOut = true;
            conn.out.address(task->sender.address());
        }
        conn.out.port(port);
    }
    else if (conn.specialOut) {
        conn.out.port(task->sender.port());
    }

    if (vers < config_->getMinCompatibleVersion()) {
        sendRegistrationRefusal(conn, RegistrationRefuseReasons::BadClientVersion);
        return true;
    }

    RemoteNodePtr ptr = getPackSenderEntry(conn.getOut());
    uint64_t local_uuid = node_->getBlockChain().uuid();
    if (local_uuid != 0 && remote_uuid != 0 && local_uuid != remote_uuid) {
       sendRegistrationRefusal(conn, RegistrationRefuseReasons::IncompatibleBlockchain);
       ptr->setBlackListed(true);
       return true;
    }
    else {
       ptr->setBlackListed(false);
    }

    iPackStream_ >> conn.id;
    iPackStream_ >> conn.key;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    nh_.gotRegistration(std::move(conn), sender);
    return true;
}

bool Transport::gotRegistrationConfirmation(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
    cslog() << "Got registration confirmation from " << task->sender;

    ConnectionId myCId;
    ConnectionId realCId;
    cs::PublicKey key;
    iPackStream_ >> myCId >> realCId >> key;

    if (!iPackStream_.good()) {
        return false;
    }

    nh_.gotConfirmation(myCId, realCId, task->sender, key, sender);
    return true;
}

bool Transport::gotRegistrationRefusal(const TaskPtr<IPacMan>& task, RemoteNodePtr&) {
    cslog() << "Got registration refusal from " << task->sender;

    RegistrationRefuseReasons reason;
    Connection::Id id;
    iPackStream_ >> id >> reason;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    nh_.gotRefusal(id);

    std::string reason_info;
    switch (reason) {
    case RegistrationRefuseReasons::BadClientVersion:
        reason_info = "incompatible node version";
        break;
    case RegistrationRefuseReasons::IncompatibleBlockchain:
        reason_info = "incompatible blockchain version";
        break;
    case RegistrationRefuseReasons::LimitReached:
        reason_info = "maximum connections limit on remote node is reached";
        break;
    default:
        {
            std::ostringstream os;
            os << "reason code " << static_cast<int>(reason);
            reason_info = os.str();
        }
        break;
    }
    cslog() << "Registration to " << task->sender << " refused: " << reason_info;

    return true;
}

bool Transport::gotSSRegistration(const TaskPtr<IPacMan>& task, RemoteNodePtr& rNode) {
    if (!(ssStatus_ == SSBootstrapStatus::Requested || ssStatus_ == SSBootstrapStatus::RegisteredWait)) {
        cswarning() << "Unexpected start node response " << static_cast<int>(ssStatus_) << " instead of Requested";
        return false;
    }
    
    cslog() << "Connection to the start node has been established";
    nh_.addSignalServer(task->sender, ssEp_, rNode);

    constexpr int MinRegistrationSize = 1 + cscrypto::kPublicKeySize;
    size_t msg_size = task->pack.getMsgSize();

    if (msg_size > MinRegistrationSize) {
        if (!parseSSSignal(task)) {
            cswarning() << "Bad start node response";
            return false;
        }
    }
    else {
        ssStatus_ = SSBootstrapStatus::RegisteredWait; // waiting for dispatch
    }

    return true;
}

bool Transport::gotSSReRegistration() {
    cswarning() << "ReRegistration on start node";

    {
        cs::Lock lock(oLock_);
        formSSConnectPack(myPublicKey_, node_->getBlockChain().uuid());
        net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
    }

    return true;
}

bool Transport::gotSSDispatch(const TaskPtr<IPacMan>& task) {
    if (ssStatus_ != SSBootstrapStatus::RegisteredWait) {
        cswarning() << "Unexpected start node response " << static_cast<int>(ssStatus_) << " instead of RegisteredWait";
        return false;
    }

    if (!parseSSSignal(task)) {
        cswarning() << "Bad start node response";
        return false;
    }

    return true;
}

bool Transport::gotSSRefusal(const TaskPtr<IPacMan>&) {
    uint16_t expectedVersion;
    RegistrationRefuseReasons reason;
    iPackStream_ >> expectedVersion >> reason;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    cserror() << "The Starter node has refused the registration";
    switch (reason) {
    case RegistrationRefuseReasons::BadClientVersion:
        cserror() << "Your client version << " << NODE_VERSION << " is obsolete. The allowed version is " << expectedVersion << ". Please upgrade your node application";
        break;
    case RegistrationRefuseReasons::IncompatibleBlockchain:
        cserror() << "Your blockchain version is incompatible. You only may join the network with empty blockchain";
        break;
    default:
        cserror() << "The reason code is " << static_cast<int>(reason);
        break;
    }

    return true;
}

bool Transport::gotSSPingWhiteNode(const TaskPtr<IPacMan>& task) {
	// MUST NOT call nh_ under oLock_!!!
	const auto nh_size = nh_.size();

    cs::Lock lock(oLock_);
    oPackStream_.init(task->pack);
    oPackStream_ << nh_size;

    Connection conn;
    conn.in = task->sender;
    conn.specialOut = false;
    
    sendDirect(oPackStream_.getPackets(), conn);
    return true;
}

bool Transport::gotSSLastBlock(const TaskPtr<IPacMan>& task, cs::Sequence lastBlock, const csdb::PoolHash& lastHash, bool canBeTrusted) {
#if !defined(MONITOR_NODE) && !defined(WEB_WALLET_NODE)
    csdebug() << "TRANSPORT> Got SS Last Block: " << lastBlock;
    csunused(task);

    Connection conn;
    conn.in = net_->resolve(config_->getSignalServerEndpoint());
    conn.specialOut = false;

    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::SSLastBlock <<  NODE_VERSION;

    cs::Hash lastHash_;
    const auto hashBinary = lastHash.to_binary();
    std::copy(hashBinary.begin(), hashBinary.end(), lastHash_.begin());

    oPackStream_ << lastBlock << canBeTrusted << myPublicKey_ << lastHash_;

    sendDirect(oPackStream_.getPackets(), conn);
#else
    csunused(task);
    csunused(lastBlock);
    csunused(lastHash);
    csunused(canBeTrusted);
#endif
    return true;
}

bool Transport::gotSSNewFriends()
{
    cslog() << "Get new friends from SignalServer";
    if (ssStatus_ != SSBootstrapStatus::Complete)
    {
        cs::Lock lock(oLock_);
        formSSConnectPack(myPublicKey_, node_->getBlockChain().uuid());
        net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
        cslog() << "Sending registration request to start node because ssStatus = " + std::to_string(static_cast<int>(ssStatus_));
    }

    cs::Signature sign;
    iPackStream_ >> sign;
    
    if (!node_->gotSSMessageVerify(sign, iPackStream_.getCurrentPtr(), iPackStream_.remainsBytes()))
    {
        cswarning() << "New Friends message is incorrect: signature isn't valid";
        return false;
    }

    uint32_t ctr = nh_.size();
    uint8_t numCirc{ 0 };
    iPackStream_ >> numCirc;

    for (uint8_t i = 0; i < numCirc; ++i) {
        EndpointData ep;
        ep.ipSpecified = true;
        cs::PublicKey key;

        iPackStream_ >> ep.ip >> ep.port >> key;

        if (!iPackStream_.good()) {
            return false;
        }

        ++ctr;

        if (!std::equal(key.cbegin(), key.cend(), config_->getMyPublicKey().cbegin())) {
            if (ctr <= config_->getMaxNeighbours()) {
                nh_.establishConnection(net_->resolve(ep));
            }
        }

        if (!nh_.canHaveNewConnection()) {
            break;
        }
    }

    cslog() << "Save new friends nodes";
    return true;

}

bool Transport::gotSSUpdateServer()
{
    cslog() << "Get new friends from SignalServer";

    cs::Signature sign;
    iPackStream_ >> sign;

    if (!node_->gotSSMessageVerify(sign, iPackStream_.getCurrentPtr(), iPackStream_.remainsBytes()))
    {
        cswarning() << "Update Server message is incorrect: signature isn't valid";
        return false;
    }

    cs::RoundNumber round = 0;
    iPackStream_ >> round;

    if (const auto currentRound = cs::Conveyer::instance().currentRoundNumber(); !(currentRound <= round + DELTA_ROUNDS_VERIFY_NEW_SERVER))
    {
        cswarning() << "Server update failed rounds verification. Receive round = " << round << " node round = " << currentRound;
        return false;
    }
    
    EndpointData ep;
    iPackStream_ >> ep.ip >> ep.port;
    
    if (!nh_.updateSignalServer(net_->resolve(ep)))
    {
        cswarning() << "Don't update start node. Error updating neighbours_";
        return false;
    }
    
    // update config.ini
    if (!Config::replaceBlock("start_node", "ip = " + ep.ip.to_string(), "port = " + std::to_string(ep.port)))
    {
        cswarning() << "Don't update start node Error updating config";
        return false;
    }

    // update config variable
    node_->updateConfigFromFile();
    
    ssEp_ = net_->resolve(ep);

    cslog() << "Registration on new start node";
    {
        cs::Lock lock(oLock_);
        formSSConnectPack(myPublicKey_, node_->getBlockChain().uuid());
        net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
    }

    return true;
}

void Transport::gotPacket(const Packet& pack, RemoteNodePtr& sender) {
    if (!pack.isFragmented()) {
        return;
    }

    nh_.neighbourSentPacket(sender, pack.getHeaderHash());
}


void Transport::redirectPacket(const Packet& pack, RemoteNodePtr& sender, bool resend) {
    sendPackInform(pack, sender);

    if (!resend) {
        return;
    }

    if (pack.isNeighbors()) {
        return;  // Do not redirect packs
    }

    {
        if (!sendLarge_.load(std::memory_order_acquire)) {
            auto lock = getNeighboursLock();
            nh_.chooseNeighbours();
        }

        auto lock = getNeighboursLock();
        nh_.neighbourHasPacket(sender, pack.getHash(), false);
        sendBroadcast(&pack);
    }
}

void Transport::sendPackInform(const Packet& pack, RemoteNodePtr& sender) {
    ConnectionPtr conn = nh_.getConnection(sender);
    if (!conn) {
        return;
    }
    sendPackInform(pack, **conn);
}

void Transport::sendPackInform(const Packet& pack, const Connection& addr) {
    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::PackInform << static_cast<cs::Byte>(pack.isNeighbors()) << pack.getHash();
    sendDirect(oPackStream_.getPackets(), addr);
    oPackStream_.clear();
}

bool Transport::gotPackInform(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
    uint8_t isDirect = 0;
    cs::Hash hHash;
    iPackStream_ >> isDirect >> hHash;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    auto lock = getNeighboursLock();
    nh_.neighbourHasPacket(sender, hHash, isDirect);
    return true;
}

void Transport::sendPackRenounce(const cs::Hash& hash, const Connection& addr) {
    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);

    oPackStream_ << NetworkCommand::PackRenounce << hash;

    sendDirect(oPackStream_.getPackets(), addr);
    oPackStream_.clear();
}

bool Transport::gotPackRenounce(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
    cs::Hash hHash;

    iPackStream_ >> hHash;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    nh_.neighbourSentRenounce(sender, hHash);

    return true;
}

void Transport::askForMissingPackages() {
    typename decltype(uncollected_)::const_iterator ptr;
    MessagePtr msg;
    size_t i = 0;

    // magic numbers??
    const uint64_t maxMask = 1ull << 63;

    cs::Lock lock(uLock_);
    ptr = uncollected_.begin();

    while (true) {
        if (i >= uncollected_.size()) {
            break;
        }

        msg = *ptr;
        ++ptr;
        ++i;

        {
            cs::Lock messageLock(msg->pLock_);

            uint16_t start = 0;
            uint64_t mask = 0;
            uint64_t req = 0;
            uint16_t end = static_cast<uint16_t>(msg->packets_.size());

            for (uint16_t j = 0; j < end; j++) {
                if (!msg->packets_[j]) {
                    if (!mask) {
                        mask = 1;
                        start = j;
                    }
                    req |= mask;
                }

                if (mask == maxMask) {
                    requestMissing(msg->headerHash_, start, req);

                    if (j > msg->maxFragment_ && j >= 128) {
                        break;
                    }

                    mask = 0;
                }
                else {
                    mask <<= 1;
                }
            }

            if (mask) {
                requestMissing(msg->headerHash_, start, req);
            }
        }
    }
}

void Transport::requestMissing(const cs::Hash& hash, const uint16_t start, const uint64_t req) {
    Packet p;

    {
        cs::Lock lock(oLock_);
        oPackStream_.init(BaseFlags::NetworkMsg);
        oPackStream_ << NetworkCommand::PackRequest << hash << start << req;
        p = *(oPackStream_.getPackets());
        oPackStream_.clear();
    }

    ConnectionPtr requestee = nh_.getNextRequestee(hash);

    if (requestee) {
        sendDirect(&p, **requestee);
    }
}

void Transport::registerMessage(MessagePtr msg) {
    cs::Lock lock(uLock_);
    auto& ptr = uncollected_.emplace(msg);
    //DEBUG:
    [[maybe_unused]] Message& message = *ptr.get();
}

bool Transport::gotPackRequest(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
    ConnectionPtr conn = nh_.getConnection(sender);
    if (!conn) {
        return false;
    }

    auto ep = conn->specialOut ? conn->out : conn->in;

    cs::Hash hHash;
    uint16_t start = 0u;
    uint64_t req = 0u;

    iPackStream_ >> hHash >> start >> req;

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }

    uint32_t reqd = 0, snt = 0;
    uint64_t mask = 1;

    while (mask) {
        if (mask & req) {
            ++reqd;
            if (net_->resendFragment(hHash, start, ep)) {
                ++snt;
            }
        }

        ++start;
        mask <<= 1;
    }

    return true;
}

// Turn on testing blockchain ID in PING packets to prevent nodes from confuse alien ones
#define PING_WITH_BCHID

void Transport::sendPingPack(const Connection& conn) {
    cs::Sequence seq = node_->getBlockChain().getLastSeq();
    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::Ping << conn.id << seq << myPublicKey_;

#if defined(PING_WITH_BCHID)
        oPackStream_ << node_->getBlockChain().uuid();
#endif

    sendDirect(oPackStream_.getPackets(), conn);
    oPackStream_.clear();
}

bool Transport::gotPing(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
    Connection::Id id = 0u;
    cs::Sequence lastSeq = 0u;

    cs::PublicKey pk;
    iPackStream_ >> id >> lastSeq >> pk;

#if defined(PING_WITH_BCHID)
    uint64_t remoteUuid = 0;
    iPackStream_ >> remoteUuid;

    auto uuid = node_->getBlockChain().uuid();

    if (uuid != 0 && remoteUuid != 0) {
        if (uuid != remoteUuid) {
            return false;   // remote is incompatible
        }
    }
#endif

    if (!iPackStream_.good() || !iPackStream_.end()) {
        return false;
    }


    if (lastSeq > maxBlock_) {
        maxBlock_ = lastSeq;
        maxBlockCount_ = 1;
    }

    if (nh_.validateConnectionId(sender, id, task->sender, pk, lastSeq)) {
        emit pingReceived(lastSeq, pk);
    }

    return true;
}

bool Transport::isOwnNodeTrusted() const {
    return (node_->getNodeLevel() != Node::Level::Normal);
}

bool Transport::gotSSIntroduceConsensusReply()
{
    csdebug() << "Get confidants from SignalServer";
    if (ssStatus_ != SSBootstrapStatus::Complete) {
        return false;
    }
    nh_.removeConfidants();

    uint8_t numCirc{ 0 };
    iPackStream_ >> numCirc;

    for (uint8_t i = 0; i < numCirc; ++i) {
        EndpointData ep;
        ep.ipSpecified = true;
        cs::PublicKey key;

        iPackStream_ >> key >> ep.ip >> ep.port;

        if (!iPackStream_.good()) {
            return false;
        }

        if (!std::equal(key.cbegin(), key.cend(), config_->getMyPublicKey().cbegin())) {
            nh_.addConfidant(net_->resolve(ep));
        }
    }
/*
    cs::Signature sign;
    iPackStream_ >> sign;

    if (!node_->gotSSMessageVerify(sign, iPackStream_.getCurrentPtr(), iPackStream_.remainsBytes()))
    {
        cswarning() << "New Friends message is incorrect: signature isn't valid";
        return false;
    }
*/
    return true;
}

void Transport::sendSSIntroduceConsensus(const std::vector<cs::PublicKey>& keys) {
    if (keys.size() == 0) return;
    ssEp_ = net_->resolve(config_->getSignalServerEndpoint());
    cslog() << "Send IntroduceConsensus to Signal Server on " << ssEp_;

    cs::Lock lock(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::IntroduceConsensus << node_->getBlockChain().uuid();
    oPackStream_ << static_cast<uint8_t>(keys.size());
    std::for_each(keys.cbegin(), keys.cend(), [this](const cs::PublicKey& key) { oPackStream_ << key; });
    net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
}

bool Transport::isConfidants() {
    return nh_.isConfidants();
}

void Transport::removeConfidants() {
    return nh_.removeConfidants();
}
