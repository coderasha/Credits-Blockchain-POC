////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                    Created by Analytical Solytions Core Team 07.09.2018                                //
////////////////////////////////////////////////////////////////////////////////////////////////////////////
#ifndef GENERALS_HPP
#define GENERALS_HPP

#include <blake2-impl.h>
#include <blake2.h>
#include <map>

#include <string>
#include <vector>

#include <csdb/csdb.h>
#include <csdb/pool.h>

#include <csnode/node.hpp>
#include <lib/system/keys.hpp>

namespace cs {
class Solver;
class Generals {
 public:
  Generals()  = default;
  ~Generals() = default;

  Generals(const Generals&) = delete;
  Generals& operator=(const Generals&) = delete;

  Hash_ buildvector(csdb::Pool& _pool, csdb::Pool& new_pool);

  void addvector(HashVector vector);
  void addmatrix(HashMatrix matrix, const std::vector<PublicKey>& confidantNodes);

  // take desision
  uint8_t       take_decision(const std::vector<PublicKey>& confidantNodes, const csdb::PoolHash& lasthash);
  static int8_t extractRaisedBitsCount(const csdb::Amount& amount);
  HashMatrix    getMatrix() const;

  void addSenderToMatrix(uint8_t myConfNum);

  const Characteristic& getCharacteristic() const;
  const PublicKey& getWriterPublicKey() const;

 private:
  struct hash_weight {
    char    a_hash[HASH_LENGTH] = {};
    uint8_t a_weight            = 0;
  };

  HashMatrix                   m_hMatrix;
  std::array<uint8_t, 10000>   m_find_untrusted;
  std::array<uint8_t, 100>     m_new_trusted;
  std::array<hash_weight, 100> m_hw_total;

  Characteristic m_characteristic;
  PublicKey m_writerPublicKey;
};
}  // namespace cs
#endif
