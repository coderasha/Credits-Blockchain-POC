#ifndef TRANSACTIONSINDEX_HPP
#define TRANSACTIONSINDEX_HPP

#include <memory>
#include <string>

#include <csdb/address.hpp>
#include <lib/system/common.hpp>
#include <lib/system/mmappedfile.hpp>
#include <lmdb.hpp>

class BlockChain;

namespace csdb {
class Pool;
} // namespace csdb

namespace cs {

class TransactionsIndex {
public:
    TransactionsIndex(BlockChain&, const std::string& _path, bool _recreate = false);
    ~TransactionsIndex() {}

    void update(const csdb::Pool&);

    Sequence getPrevTransBlock(const csdb::Address& _addr, Sequence _curr) const;

    void invalidate();
    void close();

public slots:
    void onStartReadFromDb(Sequence _lastWrittenPoolSeq);
    void onReadFromDb(const csdb::Pool&);
    void onDbReadFinished();
    void onRemoveBlock(const csdb::Pool&);

private slots:
    void onDbFailed(const LmdbException&);

private:
    void init();
    void updateFromNextBlock(const csdb::Pool&);
    static bool hasToRecreate(const std::string&, cs::Sequence&);
    void updateLastIndexed();

    void reset();

    void setPrevTransBlock(const PublicKey&, cs::Sequence _curr, cs::Sequence _prev);
    void removeLastTransBlock(const PublicKey&, cs::Sequence _curr);

    BlockChain& bc_;
    const std::string rootPath_;
    std::unique_ptr<Lmdb> db_;
    Sequence lastIndexedPool_;
    bool recreate_;
    MMappedFileWrap<FileSink> lastIndexedFile_;

    std::map<csdb::Address, cs::Sequence> lapoos_;
};
} // namespace cs
#endif // TRANSACTIONSINDEX_HPP
