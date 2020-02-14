//
// BothKeyStore.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "BothKeyStore.hh"
#include "RecordEnumerator.hh"
#include <memory>

namespace litecore {
    using namespace std;

    BothKeyStore::BothKeyStore(KeyStore *liveStore, KeyStore *deadStore)
    :KeyStore(liveStore->dataFile(), liveStore->name(), liveStore->capabilities())
    ,_liveStore(liveStore)
    ,_deadStore(deadStore)
    {
        deadStore->shareSequencesWith(*liveStore);
    }


    uint64_t BothKeyStore::recordCount(bool includeDeleted) const {
        auto count = _liveStore->recordCount(true);  // true is faster, and there are none anyway
        if (includeDeleted)
            count += _deadStore->recordCount(true);
        return count;
    }


    sequence_t BothKeyStore::set(slice key, slice version, slice value,
                                 DocumentFlags flags,
                                 Transaction &t,
                                 const sequence_t *replacingSequence,   // MVCC if non-null
                                 bool newSequence)
    {
        bool deleting = (flags & DocumentFlags::kDeleted);
        auto target = (deleting ? _deadStore : _liveStore).get();   // the store to update
        auto other  = (deleting ? _liveStore : _deadStore).get();

        sequence_t seq;
        if (!replacingSequence) {
            // Overwrite: just set, and del from other store:
            seq = target->set(key, version, value, flags, t);
            if (seq > 0)
                other->del(key, t);

        } else {
            // MVCC:
            if (*replacingSequence == 0) {
                // Request should succeed only if doc _doesn't_ exist yet, so check other KeyStore too:
                bool exists = false;
                other->get(key, kMetaOnly, [&](const Record &rec) {
                    exists = rec.exists();
                });
                if (exists)
                    return 0;
            }

            seq = target->set(key, version, value, flags, t, replacingSequence, newSequence);

            if (seq == 0 && *replacingSequence > 0) {
                // Conflict. Maybe record is currently in the other KeyStore; if so, delete it
                Assert(newSequence);
                if (other->del(key, t, *replacingSequence))
                    seq = target->set(key, version, value, flags, t);
            }
        }
        return seq;
    }


    std::vector<alloc_slice> BothKeyStore::withDocBodies(const std::vector<slice> &docIDs,
                                                         WithDocBodyCallback callback)
    {
        // First, delegate to the live store:
        size_t nDocs = docIDs.size();
        auto result = _liveStore->withDocBodies(docIDs, callback);

        // Collect the docIDs that weren't found in the live store:
        std::vector<slice> recheckDocs;
        std::vector<size_t> recheckIndexes(nDocs);
        size_t nRecheck = 0;
        for (size_t i = 0; i < nDocs; ++i) {
            if (!result[i]) {
                recheckDocs.push_back(docIDs[i]);
                recheckIndexes[nRecheck++] = i;
            }
        }

        // Retry those docIDs in the dead store and add any results:
        if (nRecheck > 0) {
            auto dead = _deadStore->withDocBodies(recheckDocs, callback);
            for (size_t i = 0; i < nRecheck; ++i) {
                if (dead[i])
                    result[recheckIndexes[i]] = dead[i];
            }
        }

        return result;
    }


    expiration_t BothKeyStore::nextExpiration() {
        auto lx = _liveStore->nextExpiration();
        auto dx = _deadStore->nextExpiration();
        if (lx > 0 && dx > 0)
            return std::min(lx, dx);        // choose the earliest time
        else
            return std::max(lx, dx);        // or choose the nonzero time
    }


#pragma mark - ENUMERATOR:


    template <typename T>
    static inline int compare(T a, T b) {
        return (a < b) ? -1 : ((a > b) ? 1 : 0);
    }


    // Enumerator implementation for BothKeyStore. It enumerates both KeyStores in parallel,
    // always returning the lowest-sorting record (basically a merge-sort.)
    class BothEnumeratorImpl : public RecordEnumerator::Impl {
    public:
        BothEnumeratorImpl(bool bySequence,
                           sequence_t since,
                           RecordEnumerator::Options options,
                           KeyStore *liveStore, KeyStore *deadStore)
        :_liveImpl(liveStore->newEnumeratorImpl(bySequence, since, options))
        ,_deadImpl(deadStore->newEnumeratorImpl(bySequence, since, options))
        ,_bySequence(bySequence)
        ,_descending(options.sortOption == kDescending)
        { }

        virtual bool next() override {
            // Advance the enumerator with the lowest key, or both if they're equal:
            if (_cmp <= 0) {
                if (!_liveImpl->next())
                    _liveImpl.reset();
            }
            if (_cmp >= 0) {
                if (!_deadImpl->next())
                    _deadImpl.reset();
            }

            // Compare the enumerators' keys or sequences:
            if (_liveImpl && _deadImpl) {
                if (_bySequence)
                    _cmp = compare(_liveImpl->sequence(), _deadImpl->sequence());
                else
                    _cmp = _liveImpl->key().compare(_deadImpl->key());
            } else if (_liveImpl) {
                _cmp = -1;
            } else if (_deadImpl) {
                _cmp = 1;
            } else {
                // finished
                _cmp = 0;
                _current = nullptr;
                return false;
            }

            if (_descending)
                _cmp = -_cmp;

            // Pick the enumerator with the lowest key/sequence to be used next.
            // In case of a tie, pick the live one since it has priority.
            _current = ((_cmp <= 0) ? _liveImpl : _deadImpl).get();
            return true;
        }

        virtual bool read(Record &record) const override    {return _current->read(record);}
        virtual slice key() const override                  {return _current->key();}
        virtual sequence_t sequence() const override        {return _current->sequence();}

    private:
        unique_ptr<RecordEnumerator::Impl> _liveImpl, _deadImpl;    // Real enumerators
        RecordEnumerator::Impl* _current {nullptr};                 // Enumerator w/lowest key
        int _cmp {0};                                               // Comparison of live to dead
        bool _bySequence, _descending;                              // Sorting by sequence?
    };


    RecordEnumerator::Impl* BothKeyStore::newEnumeratorImpl(bool bySequence,
                                                            sequence_t since,
                                                            RecordEnumerator::Options options)
    {
        if (options.includeDeleted) {
            if (options.sortOption == kUnsorted)
                options.sortOption = kAscending;    // we need ordering to merge
            return new BothEnumeratorImpl(bySequence, since, options,
                                          _liveStore.get(), _deadStore.get());
        } else {
            options.includeDeleted = true;  // no need for enum to filter out deleted docs
            return _liveStore->newEnumeratorImpl(bySequence, since, options);
        }
    }


}