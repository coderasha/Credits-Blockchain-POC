#ifndef CSCRYPTO_HPP
#define CSCRYPTO_HPP

#include <cinttypes>
#include <cstddef>
#include <array>
#include <vector>

namespace cscrypto {

const size_t kHashSize = 32;
const size_t kPublicKeySize = 32;
const size_t kPrivateKeySize = 64;
const size_t kSignatureSize = 64;

using Byte = uint8_t;

template <std::size_t size>
using ByteArray = std::array<cscrypto::Byte, size>;
using Bytes = std::vector<cscrypto::Byte>;

using Hash = cscrypto::ByteArray<kHashSize>;
using PublicKey = cscrypto::ByteArray<kPublicKeySize>;
using PrivateKey = cscrypto::ByteArray<kPrivateKeySize>;
using Signature = cscrypto::ByteArray<kSignatureSize>;

void CalculateHash(Hash& hash, const Byte* data, size_t data_size);

void GenerateKeyPair(PublicKey& public_key, PrivateKey& private_key);

bool ValidateKeyPair(const PublicKey& public_key, const PrivateKey& private_key);

void GenerateSignature(Signature& signature, const PrivateKey& private_key,
                       const Byte* data, size_t data_size);

bool VerifySignature(const Signature& signature, const PublicKey& public_key,
                     const Byte* data, size_t data_size);

bool VerifySignature(const Byte* signature, const Byte* public_key,
  const Byte* data, size_t data_size);

}  // namespace cscrypto

#endif  // CSCRYPTO_HPP
