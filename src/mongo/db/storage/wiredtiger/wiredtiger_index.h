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

#include <boost/shared_ptr.hpp>

#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"

#pragma once

namespace mongo {
    class IndexCatalogEntry;

    /**
     * Caller takes ownership.
     * All permanent data will be stored and fetch from dataInOut.
     */
    SortedDataInterface* getWiredTigerIndex(
            WiredTigerDatabase &db, const std::string &ns,
            const std::string &idxName, IndexCatalogEntry& info,
            boost::shared_ptr<void>* dataInOut);

    class WiredTigerIndex : public SortedDataInterface {
        public:
        static std::string _getURI(const std::string &ns, const std::string &idxName) {
            return "table:" + ns + ".$" + idxName;
        }
        static int Create(WiredTigerDatabase &db,
                const std::string &ns, const std::string &idxName, IndexCatalogEntry& info);

        WiredTigerIndex(WiredTigerDatabase &db, const IndexCatalogEntry& info,
                const std::string &ns, const std::string &idxName)
            : _db(db), _info(info), _uri(_getURI(ns, idxName)) {}

        virtual SortedDataBuilderInterface* getBulkBuilder(
                OperationContext* txn, bool dupsAllowed);

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed);

        virtual bool unindex(OperationContext* txn, const BSONObj& key, const DiskLoc& loc);

        virtual void fullValidate(OperationContext* txn, long long *numKeysOut);

        virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const DiskLoc& loc);

        virtual bool isEmpty();

        virtual Status touch(OperationContext* txn) const;
        
        virtual long long getSpaceUsedBytes( OperationContext* txn ) const;

        bool isDup(OperationContext *txn, const BSONObj& key, DiskLoc loc);

        virtual SortedDataInterface::Cursor* newCursor(
                OperationContext* txn, int direction) const;

        virtual Status initAsEmpty(OperationContext* txn);

        WT_CURSOR *GetCursor(WiredTigerSession &session, bool acquire=false) const;
        const std::string &GetURI() const;

        private:
            class IndexCursor : public SortedDataInterface::Cursor {
                public:
                IndexCursor(WT_CURSOR *cursor,
                        WiredTigerSession &session, OperationContext *txn, bool forward)
                   : _cursor(cursor, session, true), // XXX cursor owns session
                     _txn(txn),
                     _forward(forward),
                     _eof(true) {
                }

                virtual ~IndexCursor() {
                    
                }

                virtual int getDirection() const;

                virtual bool isEOF() const;

                virtual bool pointsToSamePlaceAs(
                        const SortedDataInterface::Cursor &genother) const;

                virtual void aboutToDeleteBucket(const DiskLoc& bucket);

		bool _locate(const BSONObj &key, const DiskLoc& loc);

                virtual bool locate(const BSONObj &key, const DiskLoc& loc);

                virtual void customLocate(const BSONObj& keyBegin,
                                          int keyBeginLen,
                                          bool afterKey,
                                          const vector<const BSONElement*>& keyEnd,
                                          const vector<bool>& keyEndInclusive);

                void advanceTo(const BSONObj &keyBegin,
                               int keyBeginLen,
                               bool afterKey,
                               const vector<const BSONElement*>& keyEnd,
                               const vector<bool>& keyEndInclusive);

                virtual BSONObj getKey() const;

                virtual DiskLoc getDiskLoc() const;

                virtual void advance();

                virtual void savePosition();

                virtual void restorePosition();

            private:
                WiredTigerCursor _cursor;
                OperationContext *_txn;
                bool _forward;
                bool _eof;

                // For save/restorePosition since _it may be invalidated durring a yield.
                bool _savedAtEnd;
                BSONObj _savedKey;
                DiskLoc _savedLoc;
        };

        WiredTigerDatabase& _db;
        const IndexCatalogEntry& _info;
        std::string _uri;
    };
} // namespace