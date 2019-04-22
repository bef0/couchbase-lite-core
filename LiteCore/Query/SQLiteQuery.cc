//
// SQLiteQuery.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Logging.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include "Path.hh"
#include "Stopwatch.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"        // for unicodesn_tokenizerRunningQuery()
}

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    class SQLiteQueryEnumerator;


    // Implicit columns in full-text query result:
    enum {
        kFTSRowidCol,
        kFTSOffsetsCol
    };


    class SQLiteQuery : public Query, Logging {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression)
        :Query(keyStore)
        ,Logging(QueryLog)
        ,_json(selectorExpression)
        {
            logInfo("Compiling JSON query: %.*s", SPLAT(selectorExpression));
            QueryParser qp(keyStore);
            qp.parseJSON(selectorExpression);

            _parameters = qp.parameters();
            for (auto p = _parameters.begin(); p != _parameters.end();) {
                if (hasPrefix(*p, "opt_"))
                    p = _parameters.erase(p);       // Optional param, don't warn if it's unbound
                else
                    ++p;
            }

            _ftsTables = qp.ftsTablesUsed();
            for (auto ftsTable : _ftsTables) {
                if (!keyStore.db().tableExists(ftsTable))
                    error::_throw(error::NoSuchIndex, "'match' test requires a full-text index");
            }

            if (qp.usesExpiration())
                keyStore.addExpiration();

            string sql = qp.SQL();
            logInfo("Compiled as %s", sql.c_str());
            LogTo(SQL, "Compiled {Query#%u}: %s", getObjectRef(), sql.c_str());
            _statement.reset(keyStore.compile(sql));
            
            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _columnTitles = qp.columnTitles();
        }


        sequence_t lastSequence() const {
            return keyStore().lastSequence();
        }


        alloc_slice getMatchedText(const FullTextTerm &term) override {
            // Get the expression that generated the text
            if (_ftsTables.size() == 0)
                error::_throw(error::NoSuchIndex);
            string expr = _ftsTables[0];    // TODO: Support for multiple matches in a query

            if (!_matchedTextStatement) {
                auto &df = (SQLiteDataFile&) keyStore().dataFile();
                string sql = "SELECT * FROM \"" + expr + "\" WHERE docid=?";
                _matchedTextStatement.reset(new SQLite::Statement(df, sql));
            }

            alloc_slice matchedText;
            _matchedTextStatement->bind(1, (long long)term.dataSource); // dataSource is docid
            if (_matchedTextStatement->executeStep())
                matchedText = alloc_slice( ((SQLiteKeyStore&)keyStore()).columnAsSlice(_matchedTextStatement->getColumn(term.keyIndex)) );
            else
                Warn("FTS index %s has no row for docid %" PRIu64, expr.c_str(), term.dataSource);
            _matchedTextStatement->reset();
            return matchedText;
        }


        virtual unsigned columnCount() const noexcept override {
            return _statement->getColumnCount() - _1stCustomResultColumn;
        }


        virtual const vector<string>& columnTitles() const noexcept override {
            return _columnTitles;
        }


        string explain() override {
            stringstream result;
            // https://www.sqlite.org/eqp.html
            string query = _statement->getQuery();
            result << query << "\n\n";

            string sql = "EXPLAIN QUERY PLAN " + query;
            auto &df = (SQLiteDataFile&) keyStore().dataFile();
            SQLite::Statement x(df, sql);
            while (x.executeStep()) {
                for (int i = 0; i < 3; ++i)
                    result << x.getColumn(i).getInt() << "|";
                result << " " << x.getColumn(3).getText() << "\n";
            }

            result << '\n' << _json << '\n';
            return result.str();
        }

        virtual QueryEnumerator* createEnumerator(const Options *options) override;
        SQLiteQueryEnumerator* createEnumerator(const Options *options, sequence_t lastSeq);

        shared_ptr<SQLite::Statement> statement()   {return _statement;}
        unsigned objectRef() const                  {return getObjectRef();}   // (for logging)

        set<string> _parameters;            // Names of the bindable parameters
        vector<string> _ftsTables;          // Names of the FTS tables used
        unsigned _1stCustomResultColumn;    // Column index of the 1st column declared in JSON

    protected:
        ~SQLiteQuery() =default;
        string loggingClassName() const override    {return "Query";}

    private:
        alloc_slice _json;                                  // Original JSON form of the query
        shared_ptr<SQLite::Statement> _statement;           // Compiled SQLite statement
        unique_ptr<SQLite::Statement> _matchedTextStatement;// Gets the matched text
        vector<string> _columnTitles;                       // Titles of columns
    };


#pragma mark - QUERY ENUMERATOR:


    // Query enumerator that reads from prerecorded Fleece data generated by SQLiteQueryEnumerator.
    // Each array item is a row, which is itself an array of column values.
    class SQLiteQueryPlayback {
    public:
        SQLiteQueryPlayback(SQLiteQuery *query,
                            Doc *recording,
                            unsigned long long firstRow)
        :_query(query)
        ,_recording(recording)
        ,_iter(_recording->asArray())
        ,_firstRow(firstRow)
        { }

        int64_t firstRow() const        {return _firstRow;}

        bool hasEqualContents(const SQLiteQueryPlayback* other) const {
            return _recording->data() == other->_recording->data();
        }

        bool seek(int64_t rowIndex) {
            rowIndex -= _firstRow;
            if (rowIndex < 0)
                return false;
            rowIndex *= 2;
            auto rows = _recording->asArray();
            if (rowIndex >= rows->count())
                return false;
            _iter = Array::iterator(rows);
            _iter += (uint32_t)rowIndex;
            return true;
        }

        bool next() {
            _iter += 2;
            return !!_iter;
        }

        Array::iterator columns() const noexcept {
            Array::iterator i(_iter[0u]->asArray());
            i += _query->_1stCustomResultColumn;
            return i;
        }

        uint64_t missingColumns() const noexcept {
            return _iter[1u]->asUnsigned();
        }

        alloc_slice columnsAsJSON() const noexcept {
            return _iter->asArray()->toJSON();
        }

        const QueryEnumerator::FullTextTerms& fullTextTerms() {
            _fullTextTerms.clear();
            uint64_t dataSource = _iter->asArray()->get(kFTSRowidCol)->asInt();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            string offsets = _iter->asArray()->get(kFTSOffsetsCol)->asString().asString();
            const char *termStr = offsets.c_str();
            while (*termStr) {
                uint32_t n[4];
                for (int i = 0; i < 4; ++i) {
                    char *next;
                    n[i] = (uint32_t)strtol(termStr, &next, 10);
                    termStr = next;
                }
                _fullTextTerms.push_back({dataSource, n[0], n[1], n[2], n[3]});
                // {rowid, key #, term #, byte offset, byte length}
            }
            return _fullTextTerms;
        }

    private:
        Retained<SQLiteQuery> _query;
        Retained<Doc> _recording;
        Array::iterator _iter;
        unsigned long long _firstRow;
        QueryEnumerator::FullTextTerms _fullTextTerms;
    };



    // Reads from 'live' SQLite statement and records the results into a Fleece array,
    // which is then used as the data source of a SQLiteQueryEnum.
    class SQLiteQueryEnumerator : public QueryEnumerator,
                                  public DataFile::PreTransactionObserver,
                                  Logging
    {
    public:
        // Initialization:

        SQLiteQueryEnumerator(SQLiteQuery *query,
                              const Query::Options *options,
                              sequence_t lastSequence)
        :Logging(QueryLog)
        ,_query(query)
        ,_statement(query->statement())
        ,_nCols(_statement->getColumnCount())
        ,_documentKeys(query->keyStore().dataFile().documentKeys())
        ,_lastSequence(lastSequence)
        {
            if (options)
                _options = *options;
            logInfo("Created on {Query#%u}", query->objectRef());
            _statement->clearBindings();
            _unboundParameters = _query->_parameters;
            if (_options.paramBindings.buf)
                bindParameters(_options.paramBindings);
            if (!_unboundParameters.empty()) {
                stringstream msg;
                for (const string &param : _unboundParameters)
                    msg << " $" << param;
                Warn("Some query parameters were left unbound and will have value `MISSING`:%s",
                     msg.str().c_str());
            }

            LogStatement(*_statement);

            // Give this encoder its own SharedKeys instead of using the database's DocumentKeys,
            // because the query results might include dicts with new keys that aren't in the
            // DocumentKeys.
            auto resultKeys = retained(new SharedKeys);
            _enc.setSharedKeys(resultKeys);

            if (_options.oneShot) {
                // Observe a transaction starting, so I can finish reading the rest of the result
                // rows before the database changes out from under me.
                _query->keyStore().dataFile().addPreTransactionObserver(this);
                _observingTransaction = true;
            } else {
                fastForward();
            }
        }

        ~SQLiteQueryEnumerator() {
            try {
                endObservingTransaction();
                if (_statement)
                    _statement->reset();
            } catch (...) { }
            logInfo("Deleted");
        }

        void endObservingTransaction() {
            if (_observingTransaction) {
                _observingTransaction = false;
                _query->keyStore().dataFile().removePreTransactionObserver(this);
            }
        }

        void bindParameters(slice json) {
            alloc_slice fleeceData;
            if (json[0] == '{' && json[json.size-1] == '}')
                fleeceData = JSONConverter::convertJSON(json);
            else
                fleeceData = json;
            const Dict *root = Value::fromData(fleeceData)->asDict();
            if (!root)
                error::_throw(error::InvalidParameter);
            for (Dict::iterator it(root); it; ++it) {
                auto key = (string)it.keyString();
                _unboundParameters.erase(key);
                auto sqlKey = string("$_") + key;
                const Value *val = it.value();
                try {
                    switch (val->type()) {
                        case kNull:
                            break;
                        case kBoolean:
                        case kNumber:
                            if (val->isInteger() && !val->isUnsigned())
                                _statement->bind(sqlKey, (long long)val->asInt());
                            else
                                _statement->bind(sqlKey, val->asDouble());
                            break;
                        case kString:
                            _statement->bind(sqlKey, (string)val->asString());
                            break;
                        default: {
                            // Encode other types as a Fleece blob:
                            Encoder enc;
                            enc.writeValue(val);
                            alloc_slice asFleece = enc.finish();
                            _statement->bind(sqlKey, asFleece.buf, (int)asFleece.size);
                            break;
                        }
                    }
                } catch (const SQLite::Exception &x) {
                    if (x.getErrorCode() == SQLITE_RANGE)
                        error::_throw(error::InvalidQueryParam,
                                      "Unknown query property '%s'", key.c_str());
                    else
                        throw;
                }
            }
        }


        // Iteration:

        // Number of rows to encode at a time
        static constexpr int64_t kPageSize = 50;

        bool stepStatement() {
            if (_statement) {
                if (_statement->executeStep()) {
                    ++_rowCount;
                    return true;
                }
                // Reached end of result set:
                _statement->reset();
                _statement.reset(); // set to null
                endObservingTransaction();
            }
            return false;
        }

        bool next() override {
            if (_curEnumerator && _curEnumerator->next())
                ;
            else if (_nextEnumerator) {
                _curEnumerator = move(_nextEnumerator);
            } else {
                if (!_options.oneShot)
                    _oldEnumerator = move(_curEnumerator);      // keep it around for refresh()
                _curEnumerator = recordRows(kPageSize);
            }

            if (_curEnumerator) {
                ++_curRow;
                if (willLog(LogLevel::Verbose)) {
                    alloc_slice json = _curEnumerator->columnsAsJSON();
                    logVerbose("--> %.*s", SPLAT(json));
                }
                return true;
            } else {
                logVerbose("--> END");
                return false;
            }
        }

        fleece::impl::Array::iterator columns() const noexcept override {
            return _curEnumerator->columns();
        }

        uint64_t missingColumns() const noexcept override {
            return _curEnumerator->missingColumns();
        }

        bool hasFullText() const override {
            return !_query->_ftsTables.empty();
        }

        const FullTextTerms& fullTextTerms() override {
            return _curEnumerator->fullTextTerms();
        }


        int64_t getRowCount() const override {
            // To get the count we have to fast-forward all the way to the end
            const_cast<SQLiteQueryEnumerator*>(this)->fastForward();
            return _rowCount;
        }

        virtual void seek(int64_t rowIndex) override {
            if (rowIndex == _curRow) {
                // No-op
                return;
            }
            if (_curEnumerator && _curEnumerator->seek(rowIndex)) {
                // Within current enumerator
                _curRow = rowIndex;
                return;
            }
            if (rowIndex < _curRow) {
                // Seeking back:
                if (_curEnumerator && rowIndex + 1 == _curEnumerator->firstRow()) {
                    // To start of current enumerator
                    _curEnumerator->seek(_curEnumerator->firstRow());
                    _nextEnumerator = move(_curEnumerator);
                } else {
                    error::_throw(error::UnsupportedOperation,
                                  "One-shot query enumerator cannot seek back");
                }
            } else {
                // Seek forward past end of _curEnumerator...
                if (_nextEnumerator) {
                    // If there's a _nextEnumerator, it must have the row:
                    if (_nextEnumerator->seek(rowIndex)) {
                        _curEnumerator = move(_nextEnumerator);
                    } else {
                        error::_throw(error::InvalidParameter, "Seeking past end of query results");
                    }
                } else {
                    // Otherwise step forward:
                    _curEnumerator.reset();
                    while (_rowCount < rowIndex) {
                        if (!stepStatement())
                            error::_throw(error::InvalidParameter, "Seeking past end of query results");
                    }
                    _curEnumerator = recordRows(kPageSize);
                    if (!_curEnumerator)
                        error::_throw(error::InvalidParameter, "Seeking past end of query results");
                }
            }
            _curRow = rowIndex;
        }


        bool hasEqualContents(SQLiteQueryEnumerator *other) {
            SQLiteQueryPlayback* e1 = (_curEnumerator ? _curEnumerator : _nextEnumerator).get();
            if (!e1)
                e1 = _oldEnumerator.get();
            SQLiteQueryPlayback* e2 = (other->_curEnumerator ? other->_curEnumerator : other->_nextEnumerator).get();
            return e1->hasEqualContents(e2);
        }


        QueryEnumerator* refresh() override {
            if (_options.oneShot)
                error::_throw(error::UnsupportedOperation,
                              "One-shot query enumerator cannot refresh");
            unique_ptr<SQLiteQueryEnumerator> newEnum(
                                        _query->createEnumerator(&_options, _lastSequence) );
            if (newEnum) {
                if (!hasEqualContents(newEnum.get())) {
                    // Results have changed, so return new enumerator:
                    return newEnum.release();
                }
                // Results have not changed, but update my lastSequence before returning null:
                _lastSequence = newEnum->_lastSequence;
            }
            return nullptr;
        }


        // Recording rows:

        unique_ptr<SQLiteQueryPlayback> recordRows(int64_t maxRows) {
            if (!_statement)
                return nullptr;
            Stopwatch st;
            auto firstRow = _rowCount;
            int64_t numRows;
            Retained<Doc> recording = encodeRows(maxRows, &numRows);
            if (numRows == 0) {
                logVerbose("...no more rows");
                return nullptr;
            }
            logInfo("Recorded %lld rows (%zu bytes) in %.3fms",
                    numRows, recording->data().size, st.elapsed()*1000);
            return make_unique<SQLiteQueryPlayback>(_query, recording, firstRow);
        }

        void fastForward() {
            if (_statement) {
                logVerbose("Recording remaining result rows...");
                Assert(!_nextEnumerator);
                _nextEnumerator = const_cast<SQLiteQueryEnumerator*>(this)->recordRows(INT64_MAX);
                Assert(!_statement);
            }
        }

        void preTransaction() override {
            _observingTransaction = false;
            fastForward();
        }


        // Encoding:

        Retained<Doc> encodeRows(int64_t maxRows, int64_t *outNumRows) {
            _enc.reset();
            _enc.beginArray();
            unicodesn_tokenizerRunningQuery(true);
            try {
                uint64_t numRows;
                for (numRows = 0; numRows < maxRows; ++numRows) {
                    if (!stepStatement())
                        break;
                    encodeRow(_enc);
                }
                _enc.endArray();
                *outNumRows = numRows;
            } catch (...) {
                unicodesn_tokenizerRunningQuery(false);
                throw;
            }
            unicodesn_tokenizerRunningQuery(false);
            return _enc.finishDoc();
        }

        void encodeRow(Encoder &enc) {
                uint64_t missingCols = 0;
                enc.beginArray(_nCols);
                for (unsigned i = 0; i < _nCols; ++i) {
                    if (!encodeColumn(enc, i) && i < 64)
                        missingCols |= (1 << i);
                }
                enc.endArray();
                // Add an integer containing a bit-map of which columns are missing/undefined:
                enc.writeUInt(missingCols);
        }

        bool encodeColumn(Encoder &enc, int i) {
            SQLite::Column col = _statement->getColumn(i);
            switch (col.getType()) {
                case SQLITE_NULL:
                    enc.writeNull();
                    return false;   // this column value is missing
                case SQLITE_INTEGER:
                    enc.writeInt(col.getInt64());
                    break;
                case SQLITE_FLOAT:
                    enc.writeDouble(col.getDouble());
                    break;
                case SQLITE_BLOB: {
                    if (i >= _query->_1stCustomResultColumn) {
                        slice fleeceData {col.getBlob(), (size_t)col.getBytes()};
                        Scope fleeceScope(fleeceData, _documentKeys);
                        const Value *value = Value::fromTrustedData(fleeceData);
                        if (!value)
                            error::_throw(error::CorruptRevisionData);
                        enc.writeValue(value);
                        break;
                    }
                    // else fall through:
                case SQLITE_TEXT:
                    enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                    break;
                }
            }
            return true;
        }

    protected:
        string loggingClassName() const override    {return "QueryEnum";}

    private:
        Retained<SQLiteQuery> _query;
        Query::Options _options {};
        shared_ptr<SQLite::Statement> _statement;
        unsigned _nCols;
        set<string> _unboundParameters;
        SharedKeys* _documentKeys;
        sequence_t _lastSequence;       // DB's lastSequence at the time the query ran
        Encoder _enc;
        uint64_t _rowCount {0};
        int64_t _curRow {-1};
        unique_ptr<SQLiteQueryPlayback> _curEnumerator, _nextEnumerator, _oldEnumerator;
        bool _observingTransaction {false};
    };



    // The factory method that creates a SQLite Query.
    Retained<Query> SQLiteKeyStore::compileQuery(slice selectorExpression) {
        return new SQLiteQuery(*this, selectorExpression);
    }


    // The factory method that creates a SQLite QueryEnumerator, but only if the database has
    // changed since lastSeq.
    SQLiteQueryEnumerator* SQLiteQuery::createEnumerator(const Options *options,
                                                         sequence_t lastSeq)
    {
        // Start a read-only transaction, to ensure that the result of lastSequence() will be
        // consistent with the query results.
        ReadOnlyTransaction t(keyStore().dataFile());

        sequence_t curSeq = lastSequence();
        if (lastSeq > 0 && lastSeq == curSeq)
            return nullptr;
        return new SQLiteQueryEnumerator(this, options, curSeq);
    }

    QueryEnumerator* SQLiteQuery::createEnumerator(const Options *options) {
        return createEnumerator(options, 0);
    }

}
