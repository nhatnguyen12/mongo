// wiredtiger_recovery_unit.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {
    WiredTigerDatabase::~WiredTigerDatabase() {
        ClearCache();
        if (_conn) {
            int ret = _conn->close(_conn, NULL);
            invariantWTOK(ret);
        }
    }

    void WiredTigerDatabase::ClearCache() {
        boost::mutex::scoped_lock lk( _ctxLock );
        for (ContextVector::iterator i = _ctxCache.begin(); i != _ctxCache.end(); i++)
            delete (*i);
        _ctxCache.clear();
    }

    WiredTigerOperationContext &WiredTigerDatabase::GetContext() {
        {
            boost::mutex::scoped_lock lk( _ctxLock );
            if (!_ctxCache.empty()) {
                WiredTigerOperationContext &ctx = *_ctxCache.back();
                _ctxCache.pop_back();
                return ctx;
            }
        }

        return *new WiredTigerOperationContext(*this);
    }

    void WiredTigerDatabase::ReleaseContext(WiredTigerOperationContext &ctx) {
        // We can't safely keep cursors open across recovery units, so close them now
        ctx.CloseAllCursors();

        boost::mutex::scoped_lock lk( _ctxLock );
        _ctxCache.push_back(&ctx);
    }
}