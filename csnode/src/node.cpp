#include <Solver/Solver.hpp>

#include <datastream.h>
#include <dynamicbuffer.h>
#include <csnode/node.hpp>
#include <lib/system/logger.hpp>
#include <net/transport.hpp>

#include <base58.h>

#include <lz4.h>
#include <sodium.h>

#include <snappy.h>
#include <sodium.h>

const unsigned MIN_CONFIDANTS = 3;
const unsigned MAX_CONFIDANTS = 4;

Node::Node(const Config& config)
: myPublicKey_(config.getMyPublicKey())
, bc_(config.getPathToDB().c_str())
, solver_(new cs::Solver(this))
, transport_(new Transport(config, this))
#ifdef MONITOR_NODE
, stats_(bc_)
#endif
#ifdef NODE_API
, api_(bc_, solver_)
#endif
, packStreamAllocator_(1 << 26, 5)
, allocator_(1 << 26, 3)
, ostream_(&packStreamAllocator_, myPublicKey_) {
  good_ = init();
}

Node::~Node() {
  delete transport_;
  delete solver_;
}

bool Node::init() {
  if (!transport_->isGood())
    return false;

  if (!bc_.isGood())
    return false;

  // Create solver
  if (!solver_)
    return false;

  csdebug() << "Everything init";

  // check file with keys
  if (!checkKeysFile())
    return false;

  solver_->set_keys(myPublicForSig, myPrivateForSig);  // DECOMMENT WHEN SOLVER STRUCTURE WILL BE CHANGED!!!!
  solver_->addInitialBalance();

  return true;
}

bool Node::checkKeysFile() {
  std::ifstream pub("NodePublic.txt");    // 44
  std::ifstream priv("NodePrivate.txt");  // 88

  if (!pub.is_open() || !priv.is_open()) {
    cslog() << "\n\nNo suitable keys were found. Type \"g\" to generate or \"q\" to quit.";
    char gen_flag = 'a';
    std::cin >> gen_flag;

    if (gen_flag == 'g') {
      generateKeys();
      return true;
    } else
      return false;

  } else {
    std::string pub58, priv58;
    std::getline(pub, pub58);
    std::getline(priv, priv58);
    pub.close();
    priv.close();
    DecodeBase58(pub58, myPublicForSig);
    DecodeBase58(priv58, myPrivateForSig);
    if (myPublicForSig.size() != 32 || myPrivateForSig.size() != 64) {
      cslog() << "\n\nThe size of keys found is not correct. Type \"g\" to generate or \"q\" to quit.";
      char gen_flag = 'a';
      std::cin >> gen_flag;
      bool needGenerateKeys = gen_flag == 'g';
      if (gen_flag == 'g') {
        generateKeys();
      }
      return needGenerateKeys;
    }
    return checkKeysForSig();
  }
}

void Node::generateKeys() {
  uint8_t private_key[64], public_key[32];
  crypto_sign_ed25519_keypair(public_key, private_key);
  myPublicForSig.clear();
  myPrivateForSig.clear();
  std::string pub58, priv58;
  pub58  = EncodeBase58(myPublicForSig);
  priv58 = EncodeBase58(myPrivateForSig);

  myPublicForSig.resize(32);
  myPrivateForSig.resize(64);
  crypto_sign_ed25519_keypair(myPublicForSig.data(), myPrivateForSig.data());

  std::ofstream f_pub("NodePublic.txt");
  f_pub << EncodeBase58(myPublicForSig);
  f_pub.close();

  std::ofstream f_priv("NodePrivate.txt");
  f_priv << EncodeBase58(myPrivateForSig);
  f_priv.close();
}

bool Node::checkKeysForSig() {
  const uint8_t msg[] = {255, 0, 0, 0, 255};
  uint8_t       signature[64], public_key[32], private_key[64];
  for (int i = 0; i < 32; i++)
    public_key[i] = myPublicForSig[i];
  for (int i = 0; i < 64; i++)
    private_key[i] = myPrivateForSig[i];
  uint64_t sig_size;
  crypto_sign_ed25519_detached(signature, reinterpret_cast<unsigned long long*>(&sig_size), msg, 5, private_key);
  int ver_ok = crypto_sign_ed25519_verify_detached(signature, msg, 5, public_key);
  if (ver_ok == 0) {
    return true;
  } else {
    cslog() << "\n\nThe keys for node are not correct. Type \"g\" to generate or \"q\" to quit.";
    char gen_flag = 'a';
    std::cin >> gen_flag;
    if (gen_flag == 'g') {
      generateKeys();
      return true;
    } else
      return false;
  }
}

void Node::run(const Config&) {
  transport_->run();
}

/* Requests */
void Node::flushCurrentTasks() {
  transport_->addTask(ostream_.getPackets(), ostream_.getPacketsCount());
  ostream_.clear();
}

void Node::getRoundTable(const uint8_t* data, const size_t size, const RoundNum rNum, uint8_t type) {
  istream_.init(data, size);

  cslog() << "NODE> Get Round Table";

  if (roundNum_ < rNum || type == MsgTypes::BigBang)
    roundNum_ = rNum;
  else {
    LOG_WARN("Bad round number, ignoring");
    return;
  }
  if (!readRoundData(false))
    return;

  if (myLevel_ == NodeLevel::Main)
    if (!istream_.good()) {
      LOG_WARN("Bad round table format, ignoring");
      return;
    }

  if (myLevel_ == NodeLevel::Main) {
    if (!istream_.good()) {
      LOG_WARN("Bad round table format, ignoring");
      return;
    }
  }
  cs::RoundInfo roundInfo;
  roundInfo.round      = rNum;
  roundInfo.confidants = confidantNodes_;
  roundInfo.hashes.clear();
  roundInfo.general = mainNode_;
  transport_->clearTasks();
  onRoundStart();
  solver_->gotRound(std::move(roundInfo));
}

void Node::getBigBang(const uint8_t* data, const size_t size, const RoundNum rNum, uint8_t type) {
  uint32_t lastBlock = getBlockChain().getLastWrittenSequence();
  if (rNum > lastBlock && rNum >= roundNum_) {
    getRoundTable(data, size, rNum, type);
    solver_->setBigBangStatus(true);
  } else {
    cslog() << "BigBang else";
  }
}

// the round table should be sent only to trusted nodes, all other should received only round number and Main node ID
void Node::sendRoundTable() {
  ostream_.init(BaseFlags::Broadcast);
  ostream_ << MsgTypes::RoundTable << roundNum_ << static_cast<uint8_t>(confidantNodes_.size()) << mainNode_;

  for (auto& conf : confidantNodes_) {
    ostream_ << conf;
  }
  cslog() << "------------------------------------------  SendRoundTable  ---------------------------------------";
  cslog() << "Round " << roundNum_ << ", General: " << byteStreamToHex(mainNode_.str, 32) << "Confidants: ";
  int i = 0;
  for (auto& e : confidantNodes_) {
    if (e != mainNode_) {
      cslog() << i << ". " << byteStreamToHex(e.str, 32);
      i++;
    }
  }
  transport_->clearTasks();
  flushCurrentTasks();
}

void Node::sendRoundTableUpdated(const cs::RoundInfo& round) {
  ostream_.init(BaseFlags::Broadcast);

  ostream_ << MsgTypes::Round << round.round << round.confidants.size() << round.hashes.size() << round.general;

  for (auto& it : round.confidants)
    ostream_ << it;
  for (auto& it : round.hashes)
    ostream_ << it;

  // LOG_EVENT("Sending round table");
  cslog() << "------------------------------------------  SendRoundTable  ---------------------------------------";
  cslog() << "Round " << roundNum_ << ", General: " << byteStreamToHex(mainNode_.str, 32) << "Confidants: ";
  size_t i = 0;
  for (auto& e : confidantNodes_) {
    if (e != mainNode_) {
      cslog() << i << ". " << byteStreamToHex(e.str, 32);
      ++i;
    }
  }
  i = 0;
  cslog() << "Hashes";
  for (auto& e : round.hashes) {
    cslog() << i << ". " << e.to_string().c_str();
    i++;
  }

  transport_->clearTasks();
  flushCurrentTasks();
}

void Node::sendRoundTableRequest(size_t rNum) {
  if (rNum < roundNum_)
    return;

  cslog() << "rNum = " << rNum << ", real RoundNumber = " << roundNum_;

  ostream_.init(BaseFlags::Broadcast);
  ostream_ << MsgTypes::RoundTableRequest << roundNum_;

  cslog() << "Sending RoundTable request";

  LOG_EVENT("Sending RoundTable request");
  flushCurrentTasks();
}

void Node::getRoundTableRequest(const uint8_t* data, const size_t size, const PublicKey& sender) {
  istream_.init(data, size);
  size_t rNum;
  istream_ >> rNum;
  if (rNum >= roundNum_)
    return;

  cslog() << "NODE> Get RT request from " << byteStreamToHex(sender.str, 32);

  if (!istream_.good()) {
    LOG_WARN("Bad RoundTableRequest format");
    return;
  }
  sendRoundTable();
}

void Node::getTransaction(const uint8_t* data, const size_t size) {
  if (solver_->getIPoolClosed())
    return;

  if (myLevel_ != NodeLevel::Main && myLevel_ != NodeLevel::Writer)
    return;

  istream_.init(data, size);

  csdb::Pool pool;
  istream_ >> pool;

  cslog() << "NODE> Transactions amount got " << pool.transactions_count();

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad transactions packet format");
    return;
  }

  // LOG_EVENT("Got full package of transactions: " << pool.transactions_count());

  auto&    trx = pool.transactions();
  uint16_t i   = 0;

  for (auto& tr : trx) {
    cslog() << "NODE> Get transaction #:" << i << " from " << tr.source().to_string() << " ID= " << tr.innerID();

    solver_->gotTransaction(std::move(tr));
    i++;
  }
}

void Node::sendTransaction(const csdb::Transaction& trans) {
  ostream_.init(BaseFlags::Broadcast, mainNode_);
  ostream_ << MsgTypes::Transactions << roundNum_ << trans;

  cslog() << "Sending transactions";

  LOG_EVENT("Sending transaction");

  flushCurrentTasks();
}

void Node::getFirstTransaction(const uint8_t* data, const size_t size) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }

  istream_.init(data, size);

  csdb::Transaction trans;
  istream_ >> trans;

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad transaction packet format");
    return;
  }
  csdb::Pool pool_;
  pool_.add_transaction(trans);

  LOG_EVENT("Got first transaction, initializing consensus...");
  // solver_->gotTransactionList(std::move(pool_));
}

void Node::sendFirstTransaction(const csdb::Transaction& trans) {
  if (myLevel_ != NodeLevel::Main) {
    LOG_ERROR("Only main nodes can initialize the consensus procedure");
    return;
  }

  ostream_.init(BaseFlags::Broadcast);
  ostream_ << MsgTypes::FirstTransaction << roundNum_ << trans;

  flushCurrentTasks();
}

void Node::getTransactionsList(const uint8_t* data, const size_t size) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }

  csdb::Pool pool;
  pool = csdb::Pool{};

  cslog() << "Getting List: list size: " << size;

  if (!((size == 0) || (size > 2000000000))) {
    istream_.init(data, size);
    istream_ >> pool;

    if (!istream_.good() || !istream_.end()) {
      LOG_WARN("Bad transactions list packet format");
      pool = csdb::Pool{};
    }
    LOG_EVENT("Got full transactions list of " << pool.transactions_count());
  }
}

void Node::sendTransactionList(const csdb::Pool& pool) {  //, const PublicKey& target) {
  if ((myLevel_ == NodeLevel::Confidant) || (myLevel_ == NodeLevel::Normal)) {
    LOG_ERROR("Only main nodes can send transaction lists");
    return;
  }

  ostream_.init(BaseFlags::Fragmented | BaseFlags::Compressed | BaseFlags::Broadcast);
  composeMessageWithBlock(pool, MsgTypes::TransactionList);

  flushCurrentTasks();
}

void Node::sendVectorRequest(const PublicKey& node) {
  if (myLevel_ != NodeLevel::Confidant) {
    LOG_ERROR("Only confidant nodes can send vectors");
    return;
  }

  cslog() << "NODE> Sending vector request to  " << byteStreamToHex(node.str, 32);

  ostream_.init(BaseFlags::Signed, node);
  ostream_ << MsgTypes::ConsVectorRequest << roundNum_ << 1;
  flushCurrentTasks();
}

void Node::getVectorRequest(const uint8_t* data, const size_t size) {
  cslog() << __func__;
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }

  cslog() << "NODE> Getting vector Request from ";  // byteStreamToHex(sender.str, 32) <<

  istream_.init(data, size);

  int num;
  istream_ >> num;

  if (num == 1) {
    sendVector(solver_->getMyVector());
  }
  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad vector packet format");
    return;
  }
}

void Node::sendWritingConfirmation(const PublicKey& node) {
  if (myLevel_ != NodeLevel::Confidant) {
    LOG_ERROR("Only confidant nodes can send confirmation of the Writer");
    return;
  }

  cslog() << "NODE> Sending writing confirmation to  " << byteStreamToHex(node.str, 32);

  ostream_.init(BaseFlags::Signed, node);
  ostream_ << MsgTypes::ConsVectorRequest << roundNum_ << getMyConfNumber();
  flushCurrentTasks();
}

void Node::getWritingConfirmation(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }

  cslog() << "NODE> Getting WRITING CONFIRMATION from " << byteStreamToHex(sender.str, 32);

  istream_.init(data, size);

  uint8_t confNumber_;
  istream_ >> confNumber_;
  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad vector packet format");
    return;
  }
  if (confNumber_ < 3)
    solver_->addConfirmation(confNumber_);
}

void Node::sendTLRequest() {
  if ((myLevel_ != NodeLevel::Confidant) || (roundNum_ < 2)) {
    LOG_ERROR("Only confidant nodes need TransactionList");
    return;
  }

  cslog() << "NODE> Sending TransactionList request to  " << byteStreamToHex(mainNode_.str, 32);

  ostream_.init(BaseFlags::Signed, mainNode_);
  ostream_ << MsgTypes::ConsTLRequest << getMyConfNumber();
  flushCurrentTasks();
}

void Node::getTlRequest(const uint8_t* data, const size_t size) {
  if (myLevel_ != NodeLevel::Main) {
    LOG_ERROR("Only main nodes can send TransactionList");
    return;
  }

  cslog() << "NODE> Getting TransactionList request";  // byteStreamToHex(sender.str, 32) <<

  istream_.init(data, size);

  uint8_t num;
  istream_ >> num;

  if (!istream_.good() || !istream_.end()) {
    return;
  }
  if (num < getConfidants().size()) {
    sendMatrix(solver_->getMyMatrix());
  }
}

void Node::sendMatrixRequest(const PublicKey& node) {
  if (myLevel_ != NodeLevel::Confidant) {
    //  LOG_ERROR("Only confidant nodes can send vectors");
    return;
  }

  cslog() << "NODE> Sending vector request to  " << byteStreamToHex(node.str, 32);

  ostream_.init(BaseFlags::Signed, node);
  ostream_ << MsgTypes::ConsMatrixRequest << roundNum_ << 1;
  flushCurrentTasks();
}

void Node::getMatrixRequest(const uint8_t* data, const size_t size) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }

  cslog() << "NODE> Getting matrix Request";  //<<  byteStreamToHex(sender.str, 32)

  istream_.init(data, size);
  int num;
  istream_ >> num;
  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad vector packet format");
    return;
  }

  if (num == 1) {
    sendMatrix(solver_->getMyMatrix());
  }
}

void Node::getVector(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }
  if (myPublicKey_ == sender) {
    return;
  }
  cslog() << "NODE> Getting vector from " << byteStreamToHex(sender.str, 32);

  istream_.init(data, size);
  cs::HashVector vec;
  istream_ >> vec;
  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad vector packet format");
    return;
  }
  LOG_EVENT("Got vector");
  solver_->gotVector(std::move(vec));
}

void Node::sendVector(const cs::HashVector& vector) {
  cslog() << "NODE> 0 Sending vector ";

  if (myLevel_ != NodeLevel::Confidant) {
    LOG_ERROR("Only confidant nodes can send vectors");
    return;
  }
  ostream_.init(BaseFlags::Broadcast);
  ostream_ << MsgTypes::ConsVector << roundNum_ << vector;

  flushCurrentTasks();
}

void Node::getMatrix(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ != NodeLevel::Confidant) {
    return;
  }
  if (myPublicKey_ == sender) {
    return;
  }
  istream_.init(data, size);

  cs::HashMatrix mat;
  istream_ >> mat;

  cslog() << "NODE> Getting matrix from " << byteStreamToHex(sender.str, 32);

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad matrix packet format");
    return;
  }

  LOG_EVENT("Got matrix");
  solver_->gotMatrix(std::move(mat));
}

void Node::sendMatrix(const cs::HashMatrix& matrix) {
  cslog() << "NODE> 0 Sending matrix to ";

  if (myLevel_ != NodeLevel::Confidant) {
    LOG_ERROR("Only confidant nodes can send matrices");
    return;
  }

  cslog() << "NODE> 1 Sending matrix to ";  //<< byteStreamToHex(it.str, 32)

  ostream_.init(BaseFlags::Broadcast);
  ostream_ << MsgTypes::ConsMatrix << roundNum_ << matrix;

  flushCurrentTasks();
}

uint32_t Node::getRoundNumber() {
  return roundNum_;
}

void Node::getBlock(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ == NodeLevel::Writer) {
    LOG_WARN("Writer cannot get blocks");
    return;
  }
  istream_.init(data, size);

  csdb::Pool pool;
  istream_ >> pool;

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad block packet format");
    return;
  }
}

void Node::sendBlock(const csdb::Pool& pool) {
  if (myLevel_ != NodeLevel::Writer) {
    LOG_ERROR("Only writer nodes can send blocks");
    return;
  }

  ostream_.init(BaseFlags::Broadcast | BaseFlags::Fragmented | BaseFlags::Compressed);
  composeMessageWithBlock(pool, MsgTypes::NewBlock);

  LOG_DEBUG("Sending block of " << pool.transactions_count() << " transactions of seq " << pool.sequence()
                                << " and hash " << pool.hash().to_string() << " and ts "
                                << pool.user_field(0).value<std::string>());
  flushCurrentTasks();
}

void Node::getBadBlock(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ == NodeLevel::Writer) {
    LOG_WARN("Writer cannot get bad blocks");
    return;
  }
  istream_.init(data, size);

  csdb::Pool pool;
  istream_ >> pool;

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad block packet format");
    return;
  }

  LOG_EVENT("Got block of " << pool.transactions_count() << " transactions");
  solver_->gotBadBlockHandler(std::move(pool), sender);
}

void Node::sendBadBlock(const csdb::Pool& pool) {
  if (myLevel_ != NodeLevel::Writer) {
    LOG_ERROR("Only writer nodes can send bad blocks");
    return;
  }
  ostream_.init(BaseFlags::Broadcast | BaseFlags::Fragmented | BaseFlags::Compressed);
  composeMessageWithBlock(pool, MsgTypes::NewBadBlock);
}

// istream_ >> compressedSize >> compressed >> metaInfoPool >> maskBitsCount;
void Node::getHash(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ != NodeLevel::Writer) {
    return;
  }
  istream_.init(data, size);

  Hash hash;
  istream_ >> hash;

  if (!istream_.good() || !istream_.end()) {
    LOG_DEBUG("Bad hash packet format");
    return;
  }

  solver_->gotHash(hash, sender);
}

void Node::getTransactionsPacket(const uint8_t* data, const std::size_t size) {
  istream_.init(data, size);

  cs::TransactionsPacket packet;
  istream_ >> packet;

  cslog() << "NODE> Transactions amount got " << packet.transactions_count();

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad transactions packet format");
    return;
  }

  if (packet.hash().is_empty()) {
    LOG_ERROR("Received transaction packet hash is empty");
    return;
  }

  solver_->gotTransactionsPacket(std::move(packet));
}

void Node::getPacketHashesRequest(const uint8_t* data, const std::size_t size, const PublicKey& sender) {
  cslog() << "NODE> getPacketHashesReques ";

  istream_.init(data, size);

  uint32_t hashesCount = 0;
  istream_ >> hashesCount;

  std::vector<cs::TransactionsPacketHash> hashes;
  hashes.reserve(hashesCount);

  for (std::size_t i = 0; i < hashesCount; ++i) {
    cs::TransactionsPacketHash hash;
    istream_ >> hash;

    hashes.push_back(std::move(hash));
  }

  cslog() << "NODE> Hashes request got size: " << hashesCount;

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad packet request format");
    return;
  }

  if (hashesCount != hashes.size()) {
    LOG_ERROR("Bad hashes created");
    return;
  }

  solver_->gotPacketHashesRequest(std::move(hashes), sender);
}

void Node::getPacketHashesReply(const uint8_t* data, const std::size_t size) {
  istream_.init(data, size);

  cs::TransactionsPacket packet;
  istream_ >> packet;

  cslog() << "NODE> Transactions hashes answer amount got " << packet.transactions_count();

  if (!istream_.good() || !istream_.end()) {
    LOG_WARN("Bad transactions hashes answer packet format");
    return;
  }

  if (packet.hash().is_empty()) {
    LOG_ERROR("Received transaction hashes answer packet hash is empty");
    return;
  }

  solver_->gotPacketHashesReply(std::move(packet));
}

void Node::getRoundTableUpdated(const uint8_t* data, const size_t size, const RoundNum round) {
  cslog() << "NODE> RoundTableUpdated";

  istream_.init(data, size);
  if (round <= solver_->currentRoundNumber()) {
    return;
  }

  uint8_t confidantsCount = 0;
  istream_ >> confidantsCount;

  if (confidantsCount == 0) {
    LOG_ERROR("Bad confidants count in round table");
    return;
  }

  uint16_t hashesCount;
  istream_ >> hashesCount;

  cs::RoundInfo roundInfo;
  roundInfo.round = round;

  PublicKey general;
  istream_ >> general;

  cs::ConfidantsKeys confidants;
  confidants.reserve(confidantsCount);

  for (std::size_t i = 0; i < confidantsCount; ++i) {
    PublicKey key;
    istream_ >> key;

    confidants.push_back(std::move(key));
  }

  cs::Hashes hashes;
  hashes.reserve(hashesCount);

  for (std::size_t i = 0; i < hashesCount; ++i) {
    cs::TransactionsPacketHash hash;
    istream_ >> hash;

    hashes.push_back(std::move(hash));
  }

  if (!istream_.end() || !istream_.good()) {
    LOG_ERROR("Bad round table parsing");
    return;
  }

  roundInfo.general    = std::move(general);
  roundInfo.confidants = std::move(confidants);
  roundInfo.hashes     = std::move(hashes);
  onRoundStart();
  solver_->gotRound(std::move(roundInfo));
}

void Node::sendCharacteristic(const csdb::Pool& emptyMetaPool, const uint32_t maskBitsCount,
                              const std::vector<uint8_t>& characteristic) {
  if (myLevel_ != NodeLevel::Writer) {
    LOG_ERROR("Only writer nodes can send blocks");
    return;
  }

  cslog() << "SendCharacteristic: seq = " << emptyMetaPool.sequence();

  ostream_.init(BaseFlags::Broadcast | BaseFlags::Fragmented);
  ostream_ << MsgTypes::NewCharacteristic << roundNum_;

  cs::DynamicBuffer buffer;
  cs::DataStream    stream(*buffer, buffer.size());

  std::string time     = emptyMetaPool.user_field(0).value<std::string>();
  uint64_t    sequence = emptyMetaPool.sequence();

  stream << static_cast<uint16_t>(time.size());
  stream << time;

  stream << maskBitsCount;

  stream << static_cast<uint32_t>(characteristic.size());
  stream << characteristic << sequence;

  ostream_ << std::string(stream.data(), stream.size());

  flushCurrentTasks();

  cslog() << "SendCharacteristic: DONE size: " << stream.size();
}

void Node::getCharacteristic(const uint8_t* data, const size_t size, const PublicKey& sender) {
  cslog() << "Characteric has arrived";

  istream_.init(data, size);

  uint16_t    timeSize = 0;
  std::string time;

  uint32_t maskBitsCount;

  uint32_t             characteristicSize = 0;
  std::vector<uint8_t> characteristic;

  uint64_t sequence;

  std::string allData;

  istream_ >> allData;

  cslog() << "Characteristic all data size: " << allData.size();

  cs::DataStream stream(const_cast<char*>(allData.data()), allData.size());

  stream >> timeSize;

  time.resize(timeSize);

  stream >> time >> maskBitsCount >> characteristicSize;

  characteristic.resize(characteristicSize);

  stream >> characteristic >> sequence;

  csdb::Pool pool;

  pool.set_sequence(sequence);
  pool.add_user_field(0, time);

  cslog() << "GetCharacteristic " << pool.sequence() << " maskbitCount" << maskBitsCount;
  cslog() << "Time >> " << pool.user_field(0).value<std::string>() << "  << Time";

  std::vector<uint8_t> characteristicMask(characteristic.begin(), characteristic.end());
  solver_->applyCharacteristic(characteristicMask, maskBitsCount, pool, sender);
}

void Node::sendHash(const Hash& hash, const PublicKey& target) {
  if (myLevel_ == NodeLevel::Writer || myLevel_ == NodeLevel::Main) {
    LOG_ERROR("Writer and Main node shouldn't send hashes");
    return;
  }

  LOG_WARN("Sending hash of " << roundNum_ << " to " << byteStreamToHex(target.str, 32));

  ostream_.init(BaseFlags::Signed | BaseFlags::Encrypted, target);
  ostream_ << MsgTypes::BlockHash << roundNum_ << hash;
  flushCurrentTasks();
}

void Node::sendTransactionsPacket(const cs::TransactionsPacket& packet) {
  if (myLevel_ != NodeLevel::Normal) {
    return;
  }

  if (packet.hash().is_empty()) {
    cslog() << "Send transaction packet with empty hash failed";

    return;
  }

  ostream_.init(BaseFlags::Fragmented | BaseFlags::Compressed | BaseFlags::Broadcast);

  uint32_t    bSize;
  const void* data = const_cast<cs::TransactionsPacket&>(packet).to_byte_stream(bSize);

  cslog() << "Sending transaction packet: size: " << bSize;

  std::string compressed;
  snappy::Compress((const char*)data, bSize, &compressed);

  ostream_ << MsgTypes::TransactionPacket << compressed;

  cslog() << "Sending transaction packet: compressed size: " << compressed.size();

  cslog() << "NODE> Sending " << packet.transactions_count() << " transaction(s)";

  flushCurrentTasks();
}

void Node::sendPacketHashesRequest(const std::vector<cs::TransactionsPacketHash>& hashes) {
  if (myLevel_ == NodeLevel::Writer) {
    LOG_ERROR("Writer should has all transactions hashes");
    return;
  }

  ostream_.init(BaseFlags::Fragmented | BaseFlags::Compressed | BaseFlags::Broadcast);

  std::size_t dataSize = hashes.size() * sizeof(cs::TransactionsPacketHash) + sizeof(uint32_t);

  cs::DynamicBuffer data(dataSize);
  cs::DataStream    stream(*data, data.size());

  stream << static_cast<uint32_t>(hashes.size());

  for (const auto& hash : hashes) {
    stream << hash;
  }

  cslog() << "Sending transaction packet request: size: " << dataSize;

  std::string compressed;
  snappy::Compress(static_cast<const char*>(*data), dataSize, &compressed);

  ostream_ << MsgTypes::TransactionsPacketRequest << compressed;

  flushCurrentTasks();
}

void Node::sendPacketHashesReply(const cs::TransactionsPacket& packet, const PublicKey& sender) {
  if (packet.hash().is_empty()) {
    cslog() << "Send transaction packet reply with empty hash failed";

    return;
  }

  ostream_.init(BaseFlags::Fragmented | BaseFlags::Compressed, sender);

  uint32_t    bSize;
  const void* data = const_cast<cs::TransactionsPacket&>(packet).to_byte_stream(bSize);

  cslog() << "Sending transaction packet reply: size: " << bSize;

  std::string compressed;
  snappy::Compress(static_cast<const char*>(data), bSize, &compressed);

  ostream_ << MsgTypes::TransactionsPacketReply << compressed;

  cslog() << "Sending transaction packet reply: compressed size: " << compressed.size();

  cslog() << "NODE> Sending " << packet.transactions_count() << " transaction(s)";

  flushCurrentTasks();
}

void Node::getBlockRequest(const uint8_t* data, const size_t size, const PublicKey& sender) {
  if (myLevel_ != NodeLevel::Normal && myLevel_ != NodeLevel::Confidant) {
    return;
  }
  if (sender == myPublicKey_) {
    return;
  }

  uint32_t requested_seq;
  istream_.init(data, size);
  istream_ >> requested_seq;

  cslog() << "GETBLOCKREQUEST> Getting the request for block: " << requested_seq;

  if (requested_seq > getBlockChain().getLastWrittenSequence()) {
    cslog() << "GETBLOCKREQUEST> The requested block: " << requested_seq << " is BEYOND my CHAIN";

    return;
  }
  solver_->gotBlockRequest(getBlockChain().getHashBySequence(requested_seq), sender);
}

void Node::sendBlockRequest(uint32_t seq) {
  if (awaitingSyncroBlock && awaitingRecBlockCount < 1) {
    cslog() << "SENDBLOCKREQUEST> New request won't be sent, we're awaiting block:  " << sendBlockRequestSequence;

    awaitingRecBlockCount++;

    return;
  }

  csdebug() << "SENDBLOCKREQUEST> Composing the request" ;

  size_t lws;
  size_t globalSequence = getBlockChain().getGlobalSequence();

  if (globalSequence == 0) {
    globalSequence = roundNum_;
  }

  lws = getBlockChain().getLastWrittenSequence();

  float syncStatus = (1.0f - (globalSequence - lws) / globalSequence) * 100.0f;
  csdebug() << "SENDBLOCKREQUEST> Syncro_Status = " << (int)syncStatus << "%";

  sendBlockRequestSequence = seq;
  awaitingSyncroBlock      = true;
  awaitingRecBlockCount    = 0;
  uint8_t requestTo        = rand() % (int)(MIN_CONFIDANTS);
  ostream_.init(BaseFlags::Signed, confidantNodes_[requestTo]);
  ostream_ << MsgTypes::BlockRequest << roundNum_ << seq;
  flushCurrentTasks();

  csdebug() << "SENDBLOCKREQUEST> Sending request for block: " << seq;
}

void Node::getBlockReply(const uint8_t* data, const size_t size) {
  cslog() << __func__;
  csdb::Pool pool;

  istream_.init(data, size);
  istream_ >> pool;

  cslog() << "GETBLOCKREPLY> Getting block " << pool.sequence();

  if (pool.sequence() == sendBlockRequestSequence) {
    cslog() << "GETBLOCKREPLY> Block Sequence is Ok";

    solver_->gotBlockReply(std::move(pool));
    awaitingSyncroBlock = false;
  } else {
    return;
  }
  if (getBlockChain().getGlobalSequence() > getBlockChain().getLastWrittenSequence()) {
    sendBlockRequest(getBlockChain().getLastWrittenSequence() + 1);
  } else {
    syncro_started = false;
  }
}

void Node::sendBlockReply(const csdb::Pool& pool, const PublicKey& sender) {
  ostream_.init(BaseFlags::Broadcast | BaseFlags::Fragmented | BaseFlags::Compressed);
  composeMessageWithBlock(pool, MsgTypes::RequestedBlock);
  flushCurrentTasks();
}

void Node::becomeWriter() {
  myLevel_ = NodeLevel::Writer;
}

void Node::onRoundStart() {
  if ((!solver_->isPoolClosed()) && (!solver_->getBigBangStatus())) {
    solver_->sendTL();
  }
  cslog() << "======================================== ROUND " << roundNum_
          << " ========================================";
  cslog() << "Node PK = " << byteStreamToHex(myPublicKey_.str, 32);

  if (mainNode_ == myPublicKey_) {
    myLevel_ = NodeLevel::Main;
  } else {
    bool    found   = false;
    uint8_t conf_no = 0;

    for (auto& conf : confidantNodes_) {
      if (conf == myPublicKey_) {
        myLevel_     = NodeLevel::Confidant;
        myConfNumber = conf_no;
        found        = true;
        break;
      }
      conf_no++;
    }

    if (!found)
      myLevel_ = NodeLevel::Normal;
  }

  // Pretty printing...

  cslog() << "Round " << roundNum_ << " started. Mynode_type:=" << myLevel_ << "Confidants: ";

  int i = 0;
  for (auto& e : confidantNodes_) {
    cslog() << i << ". " << byteStreamToHex(e.str, 32);
    i++;
  }

#ifdef SYNCRO
  if ((roundNum_ > getBlockChain().getLastWrittenSequence() + 1) || (getBlockChain().getBlockRequestNeed())) {
    sendBlockRequest(getBlockChain().getLastWrittenSequence() + 1);
    syncro_started = true;
  }
  if (roundNum_ == getBlockChain().getLastWrittenSequence() + 1) {
    syncro_started      = false;
    awaitingSyncroBlock = false;
  }
#endif
  solver_->nextRound();
  transport_->processPostponed(roundNum_);
}

bool Node::getSyncroStarted() {
  return syncro_started;
}

uint8_t Node::getMyConfNumber() {
  return myConfNumber;
}

void Node::addToPackageTemporaryStorage(const csdb::Pool& pool) {
  m_packageTemporaryStorage.push_back(pool);
}

void Node::initNextRound(const cs::RoundInfo& roundInfo) {
  roundNum_ = roundInfo.round;
  mainNode_ = roundInfo.general;
  confidantNodes_.clear();
  for (auto& conf : roundInfo.confidants) {
    confidantNodes_.push_back(conf);
  }
  sendRoundTable();
  cslog() << "NODE> RoundNumber :" << roundNum_;
  onRoundStart();
}

Node::MessageActions Node::chooseMessageAction(const RoundNum rNum, const MsgTypes type) {
  if (type == MsgTypes::BigBang && rNum > getBlockChain().getLastWrittenSequence()) {
    return MessageActions::Process;
  }
  if (type == MsgTypes::RoundTableRequest) {
    return (rNum < roundNum_ ? MessageActions::Process : MessageActions::Drop);
  }
  if (type == MsgTypes::RoundTable) {
    return (rNum > roundNum_ ? MessageActions::Process : MessageActions::Drop);
  }
  if (type == MsgTypes::BlockRequest || type == MsgTypes::RequestedBlock) {
    return (rNum <= roundNum_ ? MessageActions::Process : MessageActions::Drop);
  }
  if (rNum < roundNum_) {
    return type == MsgTypes::NewBlock ? MessageActions::Process : MessageActions::Drop;
  }
  return (rNum == roundNum_ ? MessageActions::Process : MessageActions::Postpone);
}

inline bool Node::readRoundData(const bool tail) {
  PublicKey mainNode;
  uint8_t   confSize = 0;
  istream_ >> confSize;

  cslog() << "NODE> Number of confidants :" << (int)confSize;

  if (confSize < MIN_CONFIDANTS || confSize > MAX_CONFIDANTS) {
    LOG_WARN("Bad confidants num");
    return false;
  }

  std::vector<PublicKey> confidants;
  confidants.reserve(confSize);

  istream_ >> mainNode;
  // LOG_EVENT("SET MAIN " << byteStreamToHex(mainNode.str, 32));
  while (istream_) {
    confidants.push_back(PublicKey());
    istream_ >> confidants.back();

    // LOG_EVENT("ADDED CONF " << byteStreamToHex(confidants.back().str, 32));

    if (confidants.size() == confSize && !istream_.end()) {
      if (tail)
        break;
      else {
        LOG_WARN("Too many confidant nodes received");
        return false;
      }
    }
  }

  if (!istream_.good() || confidants.size() < confSize) {
    LOG_WARN("Bad round table format, ignoring");
    return false;
  }

  std::swap(confidants, confidantNodes_);

  cslog() << "NODE> RoundNumber :" << roundNum_;

  mainNode_ = mainNode;
  return true;
}

void Node::composeMessageWithBlock(const csdb::Pool& pool, const MsgTypes type) {
  uint32_t bSize;

  const void* data = const_cast<csdb::Pool&>(pool).to_byte_stream(bSize);

  auto max    = LZ4_compressBound(bSize);
  auto memPtr = allocator_.allocateNext(max);

  auto realSize = LZ4_compress_default((const char*)data, (char*)memPtr.get(), bSize, memPtr.size());

  allocator_.shrinkLast(realSize);

  ostream_ << type << roundNum_ << bSize;

  ostream_ << std::string(static_cast<char*>(memPtr.get()), memPtr.size());
}
