/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/json.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

namespace mongo {
    static const WiredTigerItem emptyItem(NULL, 0);
    
    bool hasFieldNames(const BSONObj& obj) {
        BSONForEach(e, obj) {
            if (e.fieldName()[0])
                return true;
        }
        return false;
    }

    BSONObj stripFieldNames(const BSONObj& query) {
        if (!hasFieldNames(query))
            return query;

        BSONObjBuilder bb;
        BSONForEach(e, query) {
            bb.appendAs(e, StringData());
        }
        return bb.obj();
    }

    WiredTigerItem _toItem( const BSONObj& key ) {
        return WiredTigerItem( key.objdata(), key.objsize() );
    }

    WiredTigerItem _toItem( const DiskLoc& loc ) {
        return WiredTigerItem( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    /**
     * Constructs an IndexKeyEntry from a slice containing the bytes of a BSONObject followed
     * by the bytes of a DiskLoc
     */
    static IndexKeyEntry makeIndexKeyEntry(WT_SESSION *s, const WT_ITEM *keyCols) {
        WT_ITEM keyItem, locItem;
        int ret = wiredtiger_struct_unpack(s, keyCols->data, keyCols->size, "uu", &keyItem, &locItem);
        invariant(ret == 0);
        BSONObj key = BSONObj( static_cast<const char *>( keyItem.data ) ).getOwned();
        DiskLoc loc = *reinterpret_cast<const DiskLoc*>( locItem.data );
        return IndexKeyEntry( key, loc );
    }

    /**
     * Custom comparator used to compare Index Entries by BSONObj and DiskLoc
     */
    struct WiredTigerIndexCollator : public WT_COLLATOR {
        public:
            WiredTigerIndexCollator(const Ordering& order): _indexComparator( order ) {
                    compare = _compare;
                    terminate = _terminate;
            }

            int Compare(WT_SESSION *s, const WT_ITEM *a, const WT_ITEM *b) const {
                const IndexKeyEntry lhs = makeIndexKeyEntry(s, a);
                const IndexKeyEntry rhs = makeIndexKeyEntry(s, b);
                int cmp = _indexComparator.compare( lhs, rhs );
                if (cmp < 0)
                        cmp = -1;
                else if (cmp > 0)
                        cmp = 1;
                return cmp;
            }

            static int _compare(WT_COLLATOR *coll, WT_SESSION *s, const WT_ITEM *a, const WT_ITEM *b, int *cmp) {
                    WiredTigerIndexCollator *c = static_cast<WiredTigerIndexCollator *>(coll);
                    *cmp = c->Compare(s, a, b);
                    return 0;
            }

            static int _terminate(WT_COLLATOR *coll, WT_SESSION *s) {
                    WiredTigerIndexCollator *c = static_cast<WiredTigerIndexCollator *>(coll);
                    delete c;
                    return 0;
            }

        private:
            const IndexEntryComparison _indexComparator;
    };

    extern "C" int index_collator_customize(WT_COLLATOR *coll, WT_SESSION *s, const char *uri, WT_CONFIG_ITEM *metadata, WT_COLLATOR **collp) {
            IndexDescriptor desc(0, "unknown", fromjson(std::string(metadata->str, metadata->len)));
            *collp = new WiredTigerIndexCollator(Ordering::make(desc.keyPattern()));
            return 0;
    }

    extern "C" int index_collator_extension(WT_CONNECTION *conn, WT_CONFIG_ARG *cfg) {
            static WT_COLLATOR idx_static;

            idx_static.customize = index_collator_customize;
            return conn->add_collator(conn, "mongo_index", &idx_static, NULL);
    }

    // taken from btree_logic.cpp
    Status dupKeyError(const BSONObj& key) {
        StringBuilder sb;
        sb << "E11000 duplicate key error ";
        // sb << "index: " << _indexName << " "; // TODO
        sb << "dup key: " << key;
        return Status(ErrorCodes::DuplicateKey, sb.str());
    }

    int WiredTigerIndex::Create(WiredTigerDatabase &db,
            const std::string &ns, const std::string &idxName, IndexCatalogEntry& info) {
        WiredTigerSession swrap(db.GetSession(), db);
        WT_SESSION *s(swrap.Get());
        std::string config = "type=file,key_format=uu,value_format=u,collator=mongo_index,app_metadata=";
        config += info.descriptor()->infoObj().jsonString();
        return s->create(s, _getURI(ns, idxName).c_str(), config.c_str());
    }

    Status WiredTigerIndex::insert(OperationContext* txn,
              const BSONObj& key,
              const DiskLoc& loc,
              bool dupsAllowed) {
        invariant(!loc.isNull());
        invariant(loc.isValid());
        invariant(!hasFieldNames(key));

        // TODO optimization: save the iterator from the dup-check to speed up insert
        if (!dupsAllowed && isDup(txn, key, loc))
            return dupKeyError(key);

        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        const BSONObj& finalKey = stripFieldNames( key );
        c->set_key(c, _toItem(finalKey).Get(), _toItem(loc).Get());
        c->set_value(c, &emptyItem);
        int ret = c->insert(c);
        invariant(ret == 0);
        return Status::OK();
    }

    bool WiredTigerIndex::unindex(OperationContext* txn, const BSONObj& key, const DiskLoc& loc) {
        invariant(!loc.isNull());
        invariant(loc.isValid());
        invariant(!hasFieldNames(key));

        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        const BSONObj& finalKey = stripFieldNames( key );
        c->set_key(c, _toItem(finalKey).Get(), _toItem(loc).Get());
        int ret = c->remove(c);
        invariant(ret == 0);
        // TODO: can we avoid a search?
        const size_t numDeleted = 1;
        return numDeleted == 1;
    }

    void WiredTigerIndex::fullValidate(OperationContext* txn, long long *numKeysOut) {
        // TODO check invariants?
        *numKeysOut = 1;
    }

    Status WiredTigerIndex::dupKeyCheck(
            OperationContext* txn, const BSONObj& key, const DiskLoc& loc) {
        invariant(!hasFieldNames(key));
        if (isDup(txn, key, loc))
            return dupKeyError(key);
        return Status::OK();
    }

    bool WiredTigerIndex::isEmpty() {
        // XXX no context?
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        if (!c)
            return true;
        int ret = c->next(c);
        if (ret == WT_NOTFOUND)
            return true;
        invariant(ret == 0);
        return false;
    }

    Status WiredTigerIndex::touch(OperationContext* txn) const {
        // already in memory...
        return Status::OK();
    }
    
    long long WiredTigerIndex::getSpaceUsedBytes( OperationContext* txn ) const { return 1; }

    bool WiredTigerIndex::isDup(OperationContext *txn, const BSONObj& key, DiskLoc loc) {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor( newCursor( txn, 1 ) );
        cursor->locate(key, DiskLoc());

        return !cursor->isEOF() && cursor->getDiskLoc() != loc;
    }

    /* Cursor implementation */
    int WiredTigerIndex::IndexCursor::getDirection() const { return _forward ? 1 : -1; }

    bool WiredTigerIndex::IndexCursor::isEOF() const { return _eof; }

    bool WiredTigerIndex::IndexCursor::pointsToSamePlaceAs(
            const SortedDataInterface::Cursor &genother) const {
        const WiredTigerIndex::IndexCursor &other =
            dynamic_cast<const WiredTigerIndex::IndexCursor &>(genother);
        WT_CURSOR *c = _cursor.Get(), *otherc = other._cursor.Get();
        int cmp, ret = c->compare(c, otherc, &cmp);
        invariant(ret == 0);
        return cmp == 0;
    }

    void WiredTigerIndex::IndexCursor::aboutToDeleteBucket(const DiskLoc& bucket) {
        invariant(!"aboutToDeleteBucket should not be called");
    }

    bool WiredTigerIndex::IndexCursor::_locate(const BSONObj &key, const DiskLoc& loc) {
        WT_CURSOR *c = _cursor.Get();
        int cmp = -1, ret;
        c->set_key(c, _toItem(key).Get(), _toItem(loc).Get());
        ret = c->search_near(c, &cmp);

        // Make sure we land on a matching key
        if (ret == 0 && (_forward ? cmp < 0 : cmp > 0))
            ret = _forward ? c->next(c) : c->prev(c);
        if (ret == WT_NOTFOUND) {
            _eof = true;
            return false;
        }
        invariant(ret == 0);
        _eof = false;

        WT_ITEM keyItem, locItem;
        ret = c->get_key(c, &keyItem, &locItem);
        invariant(ret == 0);
        return key == BSONObj(static_cast<const char *>(keyItem.data));
    }

    bool WiredTigerIndex::IndexCursor::locate(const BSONObj &key, const DiskLoc& loc) {
        int ret;

        // Empty keys mean go to the beginning
        if (key.isEmpty()) {
            WT_CURSOR *c = _cursor.Get();
            ret = c->reset(c);
            invariant(ret == 0);
            advance();
            return !isEOF();
        }

        const BSONObj& finalKey = stripFieldNames( key );
        return _locate(finalKey, loc);
   }

    void WiredTigerIndex::IndexCursor::advanceTo(const BSONObj &keyBegin,
           int keyBeginLen,
           bool afterKey,
           const vector<const BSONElement*>& keyEnd,
           const vector<bool>& keyEndInclusive) {
        // XXX I think these do the same thing????

        BSONObj key = IndexEntryComparison::makeQueryObject(
                         keyBegin, keyBeginLen,
                         afterKey, keyEnd, keyEndInclusive, getDirection() );

        _locate(key, DiskLoc());
    }

    void WiredTigerIndex::IndexCursor::customLocate(const BSONObj& keyBegin,
                  int keyBeginLen,
                  bool afterKey,
                  const vector<const BSONElement*>& keyEnd,
                  const vector<bool>& keyEndInclusive) {
        advanceTo(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
    }

    BSONObj WiredTigerIndex::IndexCursor::getKey() const {
        WT_CURSOR *c = _cursor.Get();
        WT_ITEM keyItem, locItem;
        int ret = c->get_key(c, &keyItem, &locItem);
        invariant(ret == 0);
        return BSONObj(static_cast<const char *>(keyItem.data));
    }

    DiskLoc WiredTigerIndex::IndexCursor::getDiskLoc() const {
        WT_CURSOR *c = _cursor.Get();
        WT_ITEM keyItem, locItem;
        int ret = c->get_key(c, &keyItem, &locItem);
        invariant(ret == 0);
        return reinterpret_cast<const DiskLoc *>(locItem.data)[0];
    }

    void WiredTigerIndex::IndexCursor::advance() {
        WT_CURSOR *c = _cursor.Get();
        int ret = _forward ? c->next(c) : c->prev(c);
        if (ret == WT_NOTFOUND)
            _eof = true;
        else {
            invariant(ret == 0);
            _eof = false;
        }
    }

    void WiredTigerIndex::IndexCursor::savePosition() {
        if (isEOF())
            _savedAtEnd = true;
        else {
            _savedKey = getKey();
            _savedLoc = getDiskLoc();
        }
    }

    void WiredTigerIndex::IndexCursor::restorePosition() {
        if (_savedAtEnd)
            _eof = true;
        else
            locate(_savedKey, _savedLoc);
    }

    SortedDataInterface::Cursor* WiredTigerIndex::newCursor(
            OperationContext* txn, int direction) const {
        invariant((direction == 1) || (direction == -1));
        // XXX the cursor owns the session
        WiredTigerSession *session = new WiredTigerSession(_db.GetSession(), _db);
        return new IndexCursor(GetCursor(*session, true), *session, txn, direction == 1);
    }

    Status WiredTigerIndex::initAsEmpty(OperationContext* txn) {
        // No-op
        return Status::OK();
    }

    WT_CURSOR *WiredTigerIndex::GetCursor(WiredTigerSession &session, bool acquire) const {
        return session.GetCursor(GetURI(), acquire);
    }
    const std::string &WiredTigerIndex::GetURI() const { return _uri; }

    SortedDataInterface* getWiredTigerIndex(
            WiredTigerDatabase &db, const std::string &ns, const std::string &idxName,
            IndexCatalogEntry& info, boost::shared_ptr<void>* dataInOut) {
        int ret = WiredTigerIndex::Create(db, ns, idxName, info);
        invariant(ret == 0);
        return new WiredTigerIndex(db, info, ns, idxName);
    }
    
    class WiredTigerBuilderImpl : public SortedDataBuilderInterface {
    public:
        WiredTigerBuilderImpl(WiredTigerIndex &idx, OperationContext *txn, bool dupsAllowed)
                : _idx(idx), _txn(txn), _dupsAllowed(dupsAllowed), _count(0) { }

        ~WiredTigerBuilderImpl() { }

        Status addKey(const BSONObj& key, const DiskLoc& loc) {
            Status s = _idx.insert(_txn, key, loc, _dupsAllowed);
            if (s.isOK())
                _count++;
            return s;
        }

        void commit(bool mayInterrupt) { }

    private:
    WiredTigerIndex &_idx;
    OperationContext *_txn;
    bool _dupsAllowed;
        unsigned long long _count;
    };

    SortedDataBuilderInterface* WiredTigerIndex::getBulkBuilder(
            OperationContext* txn, bool dupsAllowed) {
        return new WiredTigerBuilderImpl(*this, txn, dupsAllowed);
    }

}  // namespace mongo