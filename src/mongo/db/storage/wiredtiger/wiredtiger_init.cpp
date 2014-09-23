// wiredtiger_init.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/base/init.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_engine.h"
#include "mongo/db/storage_options.h"

namespace mongo {

    namespace {
        class WiredTigerFactory : public StorageEngine::Factory {
        public:
            virtual ~WiredTigerFactory(){}
            virtual StorageEngine* create( const StorageGlobalParams& params ) const {
                return new WiredTigerEngine( params.dbpath );
            }
        };
    } // namespace

    MONGO_INITIALIZER_WITH_PREREQUISITES(WiredTigerEngineInit,
                              MONGO_DEFAULT_PREREQUISITES)(InitializerContext* context ) {
        // Some tests don't setup a global environment before we get here. Set it up now
        // so we can run WiredTiger unit tests.
        if (!hasGlobalEnvironment())
            setGlobalEnvironment(new GlobalEnvironmentMongoD());
        getGlobalEnvironment()->registerStorageEngine(
                "wiredtiger", new WiredTigerFactory() );
        return Status::OK();
    }

}

