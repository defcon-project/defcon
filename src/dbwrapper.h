// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_H
#define BITCOIN_DBWRAPPER_H

#include <clientversion.h>
#include <fs.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <memory>
#include <typeindex>
#include <type_traits>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

namespace leveldb {
class Cache;
class Env;
class FilterPolicy;
class Logger;
} // namespace leveldb

static const size_t DBWRAPPER_PREALLOC_KEY_SIZE = 64;
static const size_t DBWRAPPER_PREALLOC_VALUE_SIZE = 1024;

class dbwrapper_error : public std::runtime_error
{
public:
    explicit dbwrapper_error(const std::string& msg) : std::runtime_error(msg) {}
};

class CDBWrapper;

/** These should be considered an implementation detail of the specific database.
 */
namespace dbwrapper_private {

/** Handle database error by throwing dbwrapper_error exception.
 */
void HandleError(const leveldb::Status& status);

/** Work around circular dependency, as well as for testing in dbwrapper_tests.
 * Database obfuscation should be considered an implementation detail of the
 * specific database.
 */
const std::vector<unsigned char>& GetObfuscateKey(const CDBWrapper &w);

// Adapted from Bitcoin Core #35156: scratch streams must be empty on every exit.
class ScopedDataStreamUsage
{
    CDataStream& m_stream;
public:
    explicit ScopedDataStreamUsage(CDataStream& stream) : m_stream(stream) { assert(m_stream.empty()); }
    ~ScopedDataStreamUsage() { m_stream.clear(); }
    ScopedDataStreamUsage(const ScopedDataStreamUsage&) = delete;
    ScopedDataStreamUsage& operator=(const ScopedDataStreamUsage&) = delete;
};

std::unique_ptr<leveldb::Logger> CreateLevelDBLogger();

} // namespace dbwrapper_private

/** Batch of changes queued to be written to a CDBWrapper */
class CDBBatch
{
    friend class CDBWrapper;

private:
    const CDBWrapper &parent;
    leveldb::WriteBatch batch;

    CDataStream ssKey;
    CDataStream ssValue;

    size_t size_estimate;

public:
    /**
     * @param[in] _parent    CDBWrapper that this batch is to be submitted to
     */
    explicit CDBBatch(const CDBWrapper &_parent) : parent(_parent), ssKey(SER_DISK, CLIENT_VERSION), ssValue(SER_DISK, CLIENT_VERSION), size_estimate(0) { };

    void Clear()
    {
        batch.Clear();
        ssKey.clear();
        ssValue.clear();
        size_estimate = 0;
    }

    template <typename K, typename V>
    void Write(const K& key, const V& value)
    {
        dbwrapper_private::ScopedDataStreamUsage key_usage{ssKey};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        Write(ssKey, value);
    }

    template <typename V>
    void Write(const CDataStream& _ssKey, const V& value)
    {
        leveldb::Slice slKey((const char*)_ssKey.data(), _ssKey.size());

        dbwrapper_private::ScopedDataStreamUsage value_usage{ssValue};
        ssValue.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
        ssValue << value;
        ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
        leveldb::Slice slValue((const char*)ssValue.data(), ssValue.size());

        batch.Put(slKey, slValue);
        // - varint: key length (1 byte up to 127B, 2 bytes up to 16383B, ...)
        // - byte[]: key
        // - varint: value length
        // - byte[]: value
        // The formula below assumes the key and value are both less than 16k.
        size_estimate += 3 + (slKey.size() > 127) + slKey.size() + (slValue.size() > 127) + slValue.size();
    }

    template <typename K>
    void Erase(const K& key)
    {
        dbwrapper_private::ScopedDataStreamUsage key_usage{ssKey};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        Erase(ssKey);
    }

    void Erase(const CDataStream& _ssKey) {
        leveldb::Slice slKey((const char*)_ssKey.data(), _ssKey.size());

        batch.Delete(slKey);
        // - byte: header
        // - varint: key length
        // - byte[]: key
        // The formula below assumes the key is less than 16kB.
        size_estimate += 2 + (slKey.size() > 127) + slKey.size();
    }

    size_t SizeEstimate() const { return size_estimate; }
};

class CDBIterator
{
private:
    const CDBWrapper &parent;
    std::unique_ptr<leveldb::Iterator> piter;

public:

    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] _piter           The original leveldb iterator.
     */
    CDBIterator(const CDBWrapper &_parent, leveldb::Iterator *_piter) :
        parent(_parent), piter(_piter) { };
    ~CDBIterator();

    bool Valid() const;

    void SeekToFirst();

    template<typename K> void Seek(const K& key) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        Seek(ssKey);
    }

    void Seek(const CDataStream& ssKey) {
        leveldb::Slice slKey((const char*)ssKey.data(), ssKey.size());
        piter->Seek(slKey);
        dbwrapper_private::HandleError(piter->status());
    }

    void Next();

    template<typename K> bool GetKey(K& key) {
        try {
            CDataStream ssKey = GetKey();
            ssKey >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    CDataStream GetKey() {
        leveldb::Slice slKey = piter->key();
        return CDataStream{MakeByteSpan(slKey), SER_DISK, CLIENT_VERSION};
    }

    unsigned int GetKeySize() {
        return piter->key().size();
    }

    template<typename V> bool GetValue(V& value) {
        leveldb::Slice slValue = piter->value();
        try {
            CDataStream ssValue{MakeByteSpan(slValue), SER_DISK, CLIENT_VERSION};
            ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    unsigned int GetValueSize() {
        return piter->value().size();
    }

};

class CDBWrapper
{
    friend const std::vector<unsigned char>& dbwrapper_private::GetObfuscateKey(const CDBWrapper &w);
private:
    //! custom environment this database is using (may be nullptr in case of default environment)
    // Owners precede pdb so the database is destroyed before its dependencies.
    std::unique_ptr<leveldb::Env> penv;
    std::unique_ptr<leveldb::Cache> m_block_cache;
    std::unique_ptr<const leveldb::FilterPolicy> m_filter_policy;
    std::unique_ptr<leveldb::Logger> m_logger;

    //! database options used
    leveldb::Options options;

    //! options used when reading from the database
    leveldb::ReadOptions readoptions;

    //! options used when iterating over values of the database
    leveldb::ReadOptions iteroptions;

    //! options used when writing to the database
    leveldb::WriteOptions writeoptions;

    //! options used when sync writing to the database
    leveldb::WriteOptions syncoptions;

    //! the database itself
    std::unique_ptr<leveldb::DB> pdb;

    //! the name of this database
    std::string m_name;

    //! a key used for optional XOR-obfuscation of the database
    std::vector<unsigned char> obfuscate_key;

    //! the key under which the obfuscation key is stored
    static const std::string OBFUSCATE_KEY_KEY;

    //! the length of the obfuscate key in number of bytes
    static const unsigned int OBFUSCATE_KEY_NUM_BYTES;

    std::vector<unsigned char> CreateObfuscateKey() const;

public:
    /**
     * @param[in] path        Location in the filesystem where leveldb data will be stored.
     * @param[in] nCacheSize  Configures various leveldb cache settings.
     * @param[in] fMemory     If true, use leveldb's memory environment.
     * @param[in] fWipe       If true, remove all existing data.
     * @param[in] obfuscate   If true, store data obfuscated via simple XOR. If false, XOR
     *                        with a zero'd byte array.
     */
    CDBWrapper(const fs::path& path, size_t nCacheSize, bool fMemory = false, bool fWipe = false, bool obfuscate = false);
    ~CDBWrapper();

    CDBWrapper(const CDBWrapper&) = delete;
    CDBWrapper& operator=(const CDBWrapper&) = delete;

    template <typename K>
    bool ReadDataStream(const K& key, CDataStream& ssValue) const
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return ReadDataStream(ssKey, ssValue);
    }

    bool ReadDataStream(const CDataStream& ssKey, CDataStream& ssValue) const
    {
        leveldb::Slice slKey((const char*)ssKey.data(), ssKey.size());

        std::string strValue;
        leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
        if (!status.ok()) {
            if (status.IsNotFound())
                return false;
            LogPrintf("LevelDB read failure: %s\n", status.ToString());
            dbwrapper_private::HandleError(status);
        }
        CDataStream ssValueTmp{MakeByteSpan(strValue), SER_DISK, CLIENT_VERSION};
        ssValueTmp.Xor(obfuscate_key);
        ssValue = std::move(ssValueTmp);
        return true;
    }

    template <typename K, typename V>
    bool Read(const K& key, V& value) const
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return Read(ssKey, value);
    }

    template <typename V>
    bool Read(const CDataStream& ssKey, V& value) const
    {
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        if (!ReadDataStream(ssKey, ssValue)) {
            return false;
        }

        try {
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template <typename K, typename V>
    bool Write(const K& key, const V& value, bool fSync = false)
    {
        CDBBatch batch(*this);
        batch.Write(key, value);
        return WriteBatch(batch, fSync);
    }

    template <typename K>
    bool Exists(const K& key) const
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return Exists(ssKey);
    }

    bool Exists(const CDataStream& key) const
    {
        leveldb::Slice slKey((const char*)key.data(), key.size());

        std::string strValue;
        leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
        if (!status.ok()) {
            if (status.IsNotFound())
                return false;
            LogPrintf("LevelDB read failure: %s\n", status.ToString());
            dbwrapper_private::HandleError(status);
        }
        return true;
    }

    template <typename K>
    bool Erase(const K& key, bool fSync = false)
    {
        CDBBatch batch(*this);
        batch.Erase(key);
        return WriteBatch(batch, fSync);
    }

    bool WriteBatch(CDBBatch& batch, bool fSync = false);

    // Get an estimate of LevelDB memory usage (in bytes).
    size_t DynamicMemoryUsage() const;

    CDBIterator *NewIterator()
    {
        return new CDBIterator(*this, pdb->NewIterator(iteroptions));
    }

    /**
     * Return true if the database managed by this class contains no entries.
     */
    bool IsEmpty();

    template<typename K>
    size_t EstimateSize(const K& key_begin, const K& key_end) const
    {
        CDataStream ssKey1(SER_DISK, CLIENT_VERSION), ssKey2(SER_DISK, CLIENT_VERSION);
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        leveldb::Slice slKey1((const char*)ssKey1.data(), ssKey1.size());
        leveldb::Slice slKey2((const char*)ssKey2.data(), ssKey2.size());
        uint64_t size = 0;
        leveldb::Range range(slKey1, slKey2);
        pdb->GetApproximateSizes(&range, 1, &size);
        return size;
    }

    /**
     * Compact a certain range of keys in the database.
     */
    template<typename K>
    void CompactRange(const K& key_begin, const K& key_end) const
    {
        CDataStream ssKey1(SER_DISK, CLIENT_VERSION), ssKey2(SER_DISK, CLIENT_VERSION);
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        leveldb::Slice slKey1((const char*)ssKey1.data(), ssKey1.size());
        leveldb::Slice slKey2((const char*)ssKey2.data(), ssKey2.size());
        pdb->CompactRange(&slKey1, &slKey2);
    }

    void CompactFull() const
    {
        pdb->CompactRange(nullptr, nullptr);
    }

};

template<typename CDBTransaction>
class CDBTransactionIterator
{
private:
    CDBTransaction& transaction;

    typedef typename std::remove_pointer<decltype(transaction.parent.NewIterator())>::type ParentIterator;

    // We maintain 2 iterators, one for the transaction and one for the parent
    // At all times, only one of both provides the current value. The decision is made by comparing the current keys
    // of both iterators, so that always the smaller key is the current one. On Next(), the previously chosen iterator
    // is advanced.
    typename CDBTransaction::WritesMap::iterator transactionIt;
    std::unique_ptr<ParentIterator> parentIt;
    CDataStream parentKey;
    bool curIsParent{false};

public:
    explicit CDBTransactionIterator(CDBTransaction& _transaction) :
            transaction(_transaction),
            parentKey(SER_DISK, CLIENT_VERSION)
    {
        transactionIt = transaction.writes.end();
        parentIt = std::unique_ptr<ParentIterator>(transaction.parent.NewIterator());
    }

    void SeekToFirst() {
        transactionIt = transaction.writes.begin();
        parentIt->SeekToFirst();
        SkipDeletedAndOverwritten();
        DecideCur();
    }

    template<typename K>
    void Seek(const K& key) {
        Seek(CDBTransaction::KeyToDataStream(key));
    }

    void Seek(const CDataStream& ssKey) {
        transactionIt = transaction.writes.lower_bound(ssKey);
        parentIt->Seek(ssKey);
        SkipDeletedAndOverwritten();
        DecideCur();
    }

    bool Valid() {
        const bool parent_valid = parentIt->Valid();
        return transactionIt != transaction.writes.end() || parent_valid;
    }

    void Next() {
        if (transactionIt == transaction.writes.end() && !parentIt->Valid()) {
            return;
        }
        if (curIsParent) {
            assert(parentIt->Valid());
            parentIt->Next();
            SkipDeletedAndOverwritten();
        } else {
            assert(transactionIt != transaction.writes.end());
            ++transactionIt;
        }
        DecideCur();
    }

    template<typename K>
    bool GetKey(K& key) {
        if (!Valid()) {
            return false;
        }

        try {
            // TODO try to avoid copy transactionIt->first (we need a stream that allows reading from external buffers)
            (curIsParent ? parentKey : CDataStream{transactionIt->first}) >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    CDataStream GetKey() {
        if (!Valid()) {
            return CDataStream(SER_DISK, CLIENT_VERSION);
        }
        if (curIsParent) {
            return parentKey;
        } else {
            return transactionIt->first;
        }
    }

    unsigned int GetKeySize() {
        if (!Valid()) {
            return 0;
        }
        if (curIsParent) {
            return parentIt->GetKeySize();
        } else {
            return transactionIt->first.vKey.size();
        }
    }

    template<typename V>
    bool GetValue(V& value) {
        if (!Valid()) {
            return false;
        }
        if (curIsParent) {
            return transaction.Read(parentKey, value);
        } else {
            return transaction.Read(transactionIt->first, value);
        }
    };

private:
    void SkipDeletedAndOverwritten() {
        while (parentIt->Valid()) {
            parentKey = parentIt->GetKey();
            if (!transaction.deletes.count(parentKey) && !transaction.writes.count(parentKey)) {
                break;
            }
            parentIt->Next();
        }
    }

    void DecideCur() {
        if (transactionIt != transaction.writes.end() && !parentIt->Valid()) {
            curIsParent = false;
        } else if (transactionIt == transaction.writes.end() && parentIt->Valid()) {
            curIsParent = true;
        } else if (transactionIt != transaction.writes.end() && parentIt->Valid()) {
            if (CDBTransaction::DataStreamCmp::less(transactionIt->first, parentKey)) {
                curIsParent = false;
            } else {
                curIsParent = true;
            }
        }
    }
};

template<typename Parent, typename CommitTarget>
class CDBTransaction {
    friend class CDBTransactionIterator<CDBTransaction>;
    template <typename, typename> friend class CDBTransaction;

protected:
    Parent &parent;
    CommitTarget &commitTarget;
    ssize_t memoryUsage{0}; // signed, just in case we made an error in the calculations so that we don't get an overflow

    struct DataStreamCmp {
        static bool less(const CDataStream& a, const CDataStream& b) noexcept {
            return std::lexicographical_compare(
                    (const uint8_t*)a.data(), (const uint8_t*)a.data() + a.size(),
                    (const uint8_t*)b.data(), (const uint8_t*)b.data() + b.size());
        }
        bool operator()(const CDataStream& a, const CDataStream& b) const noexcept {
            return less(a, b);
        }
    };

    struct ValueHolder {
        size_t memoryUsage;
        explicit ValueHolder(size_t _memoryUsage) : memoryUsage(_memoryUsage) {}
        virtual ~ValueHolder() = default;
        virtual void Write(const CDataStream& ssKey, CommitTarget &parent) = 0;
    };
    typedef std::unique_ptr<ValueHolder> ValueHolderPtr;

    template <typename V>
    struct ValueHolderImpl : ValueHolder {
        ValueHolderImpl(const V &_value, size_t _memoryUsage) : ValueHolder(_memoryUsage), value(_value) {}

        virtual void Write(const CDataStream& ssKey, CommitTarget &commitTarget) override {
            // Keep this overlay value intact if staging the write throws.
            commitTarget.Write(ssKey, value);
        }

        V value;
    };

    template<typename K>
    static CDataStream KeyToDataStream(const K& key) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return ssKey;
    }

    typedef std::map<CDataStream, ValueHolderPtr, DataStreamCmp> WritesMap;
    typedef std::set<CDataStream, DataStreamCmp> DeletesSet;

    WritesMap writes;
    DeletesSet deletes;

public:
    CDBTransaction(Parent &_parent, CommitTarget &_commitTarget) : parent(_parent), commitTarget(_commitTarget) {}

    template <typename K, typename V>
    void Write(const K& key, const V& v) {
        Write(KeyToDataStream(key), v);
    }

    template <typename V>
    void Write(const CDataStream& ssKey, const V& v) {
        const size_t valueMemoryUsage = ::GetSerializeSize(v, CLIENT_VERSION);
        ValueHolderPtr valueHolder = std::make_unique<ValueHolderImpl<V>>(v, valueMemoryUsage);

        auto writeIt = writes.find(ssKey);
        const auto deleteIt = deletes.find(ssKey);
        const size_t oldMemoryUsage =
            (writeIt == writes.end() ? 0 : ssKey.size() + writeIt->second->memoryUsage) +
            (deleteIt == deletes.end() ? 0 : ssKey.size());

        if (writeIt == writes.end()) {
            const auto [it, inserted] = writes.emplace(ssKey, std::move(valueHolder));
            assert(inserted);
            writeIt = it;
        } else {
            writeIt->second.swap(valueHolder);
        }
        if (deleteIt != deletes.end()) {
            deletes.erase(deleteIt);
        }

        memoryUsage -= oldMemoryUsage;
        memoryUsage += ssKey.size() + valueMemoryUsage;
    }

    template <typename K, typename V>
    bool Read(const K& key, V& value) {
        return Read(KeyToDataStream(key), value);
    }

    template <typename V>
    bool Read(const CDataStream& ssKey, V& value) {
        if (deletes.count(ssKey)) {
            return false;
        }

        auto it = writes.find(ssKey);
        if (it != writes.end()) {
            auto *impl = dynamic_cast<ValueHolderImpl<V> *>(it->second.get());
            if (!impl) {
                throw std::runtime_error("Read called with V != previously written type");
            }
            value = impl->value;
            return true;
        }

        return parent.Read(ssKey, value);
    }

    template <typename K>
    bool Exists(const K& key) {
        return Exists(KeyToDataStream(key));
    }

    bool Exists(const CDataStream& ssKey) {
        if (deletes.count(ssKey)) {
            return false;
        }

        if (writes.count(ssKey)) {
            return true;
        }

        return parent.Exists(ssKey);
    }

    template <typename K>
    void Erase(const K& key) {
        return Erase(KeyToDataStream(key));
    }

    void Erase(const CDataStream& ssKey) {
        auto writeIt = writes.find(ssKey);
        const bool inserted = deletes.emplace(ssKey).second;
        if (inserted) {
            memoryUsage += ssKey.size();
        }
        if (writeIt != writes.end()) {
            memoryUsage -= ssKey.size() + writeIt->second->memoryUsage;
            writes.erase(writeIt);
        }
    }

    void Clear() {
        writes.clear();
        deletes.clear();
        memoryUsage = 0;
    }

    void Commit() {
        // Nested transactions use their parent transaction as the commit target.
        if constexpr (std::is_same_v<Parent, CommitTarget>) {
            // Prepare only the child delta. Any allocation or value-copy failure
            // leaves both live overlays untouched.
            CommitTarget staged(commitTarget.parent, commitTarget.commitTarget);
            for (const auto& key : deletes) {
                staged.Erase(key);
            }
            for (const auto& write : writes) {
                write.second->Write(write.first, staged);
            }

            // Apply the prepared nodes without allocating. DataStreamCmp compares
            // serialized bytes and is non-throwing, and node-handle insertion
            // reuses the staged nodes.
            auto stagedDeleteIt = staged.deletes.begin();
            while (stagedDeleteIt != staged.deletes.end()) {
                auto currentDelete = stagedDeleteIt++;
                const CDataStream& key = *currentDelete;
                const size_t keySize = key.size();
                const auto targetDeleteIt = commitTarget.deletes.find(key);
                auto targetWriteIt = commitTarget.writes.find(key);
                assert(targetDeleteIt == commitTarget.deletes.end() ||
                       targetWriteIt == commitTarget.writes.end());

                if (targetDeleteIt == commitTarget.deletes.end()) {
                    auto node = staged.deletes.extract(currentDelete);
                    const auto inserted = commitTarget.deletes.insert(std::move(node));
                    assert(inserted.inserted);
                    commitTarget.memoryUsage += static_cast<ssize_t>(keySize);
                }
                if (targetWriteIt != commitTarget.writes.end()) {
                    commitTarget.memoryUsage -= static_cast<ssize_t>(
                        keySize + targetWriteIt->second->memoryUsage);
                    commitTarget.writes.erase(targetWriteIt);
                }
            }

            auto stagedWriteIt = staged.writes.begin();
            while (stagedWriteIt != staged.writes.end()) {
                auto currentWrite = stagedWriteIt++;
                const CDataStream& key = currentWrite->first;
                const size_t keySize = key.size();
                const ssize_t valueMemoryUsage =
                    static_cast<ssize_t>(currentWrite->second->memoryUsage);
                auto targetWriteIt = commitTarget.writes.find(key);
                const auto targetDeleteIt = commitTarget.deletes.find(key);
                assert(targetWriteIt == commitTarget.writes.end() ||
                       targetDeleteIt == commitTarget.deletes.end());

                if (targetWriteIt != commitTarget.writes.end()) {
                    const ssize_t oldValueMemoryUsage =
                        static_cast<ssize_t>(targetWriteIt->second->memoryUsage);
                    targetWriteIt->second.swap(currentWrite->second);
                    commitTarget.memoryUsage += valueMemoryUsage - oldValueMemoryUsage;
                    staged.writes.erase(currentWrite);
                } else {
                    auto node = staged.writes.extract(currentWrite);
                    const auto inserted = commitTarget.writes.insert(std::move(node));
                    assert(inserted.inserted);
                    commitTarget.memoryUsage += static_cast<ssize_t>(keySize) + valueMemoryUsage;
                    if (targetDeleteIt != commitTarget.deletes.end()) {
                        commitTarget.deletes.erase(targetDeleteIt);
                        commitTarget.memoryUsage -= static_cast<ssize_t>(keySize);
                    }
                }
            }
        } else {
            for (const auto &k : deletes) {
                commitTarget.Erase(k);
            }
            for (auto &p : writes) {
                p.second->Write(p.first, commitTarget);
            }
        }
        Clear();
    }

    bool IsClean() const {
        return writes.empty() && deletes.empty();
    }

    size_t GetMemoryUsage() const {
        if (memoryUsage < 0) {
            // something went wrong when we accounted/calculated used memory...
            static volatile bool didPrint = false;
            if (!didPrint) {
                LogPrintf("CDBTransaction::%s -- negative memoryUsage (%d)\n", __func__, memoryUsage);
                didPrint = true;
            }
            return 0;
        }
        return (size_t)memoryUsage;
    }

    CDBTransactionIterator<CDBTransaction>* NewIterator() {
        return new CDBTransactionIterator<CDBTransaction>(*this);
    }
    std::unique_ptr<CDBTransactionIterator<CDBTransaction>> NewIteratorUniquePtr() {
        return std::make_unique<CDBTransactionIterator<CDBTransaction>>(*this);
    }
};

#endif // BITCOIN_DBWRAPPER_H
