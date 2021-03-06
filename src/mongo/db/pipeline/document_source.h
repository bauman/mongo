/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/pch.h"

#include <boost/optional.hpp>
#include <boost/unordered_map.hpp>
#include <deque>

#include "mongo/db/clientcursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/projection.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/s/shard.h"
#include "mongo/s/strategy.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
    class Accumulator;
    class Cursor;
    class Document;
    class Expression;
    class ExpressionFieldPath;
    class ExpressionObject;
    class DocumentSourceLimit;

    class DocumentSource : public IntrusiveCounterUnsigned {
    public:
        virtual ~DocumentSource() {}

        /** Returns the next Document if there is one or boost::none if at EOF.
         *  Subclasses must call pExpCtx->checkForInterupt().
         */
        virtual boost::optional<Document> getNext() = 0;

        /**
         * Inform the source that it is no longer needed and may release its resources.  After
         * dispose() is called the source must still be able to handle iteration requests, but may
         * become eof().
         * NOTE: For proper mutex yielding, dispose() must be called on any DocumentSource that will
         * not be advanced until eof(), see SERVER-6123.
         */
        virtual void dispose();

        /**
           Get the source's name.

           @returns the string name of the source as a constant string;
             this is static, and there's no need to worry about adopting it
         */
        virtual const char *getSourceName() const;

        /**
          Set the underlying source this source should use to get Documents
          from.

          It is an error to set the source more than once.  This is to
          prevent changing sources once the original source has been started;
          this could break the state maintained by the DocumentSource.

          This pointer is not reference counted because that has led to
          some circular references.  As a result, this doesn't keep
          sources alive, and is only intended to be used temporarily for
          the lifetime of a Pipeline::run().

          @param pSource the underlying source to use
         */
        virtual void setSource(DocumentSource *pSource);

        /**
          Attempt to coalesce this DocumentSource with its successor in the
          document processing pipeline.  If successful, the successor
          DocumentSource should be removed from the pipeline and discarded.

          If successful, this operation can be applied repeatedly, in an
          attempt to coalesce several sources together.

          The default implementation is to do nothing, and return false.

          @param pNextSource the next source in the document processing chain.
          @returns whether or not the attempt to coalesce was successful or not;
            if the attempt was not successful, nothing has been changed
         */
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        /**
          Optimize the pipeline operation, if possible.  This is a local
          optimization that only looks within this DocumentSource.  For best
          results, first coalesce compatible sources using coalesce().

          This is intended for any operations that include expressions, and
          provides a hook for those to optimize those operations.

          The default implementation is to do nothing.
         */
        virtual void optimize();

        enum GetDepsReturn {
            NOT_SUPPORTED, // This means the set should be ignored and the full object is required.
            EXHAUSTIVE, // This means that everything needed should be in the set
            SEE_NEXT, // Add the next Source's deps to the set
        };

        /** Get the fields this operation needs to do its job.
         *  Deps should be in "a.b.c" notation
         *  An empty string in deps means the whole document is needed.
         *
         *  @param deps results are added here. NOT CLEARED
         */
        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return NOT_SUPPORTED;
        }

        /** This takes dependencies from getDependencies and
         *  returns a projection that includes all of them
         */
        static BSONObj depsToProjection(const set<string>& deps);

        /** These functions take the same input as depsToProjection but are able to
         *  produce a Document from a BSONObj with the needed fields much faster.
         */
        typedef Document ParsedDeps; // See implementation for structure
        static ParsedDeps parseDeps(const set<string>& deps);
        static Document documentFromBsonWithDeps(const BSONObj& object, const ParsedDeps& deps);

        /**
         * In the default case, serializes the DocumentSource and adds it to the vector<Value>.
         *
         * A subclass may choose to overwrite this, rather than serialize,
         * if it should output multiple stages (eg, $sort sometimes also outputs a $limit).
         */

        virtual void serializeToArray(vector<Value>& array, bool explain = false) const;

        /// Returns true if doesn't require an input source (most DocumentSources do).
        virtual bool isValidInitialSource() const { return false; }

    protected:
        /**
           Base constructor.
         */
        DocumentSource(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Most DocumentSources have an underlying source they get their data
          from.  This is a convenience for them.

          The default implementation of setSource() sets this; if you don't
          need a source, override that to verify().  The default is to
          verify() if this has already been set.
        */
        DocumentSource *pSource;

        intrusive_ptr<ExpressionContext> pExpCtx;

    private:
        /**
         * Create a Value that represents the document source.
         *
         * This is used by the default implementation of serializeToArray() to add this object
         * to a pipeline being serialized. Returning a missing() Value results in no entry
         * being added to the array for this stage (DocumentSource).
         */
        virtual Value serialize(bool explain = false) const = 0;
    };

    /** This class marks DocumentSources that should be split between the merger and the shards.
     *  See Pipeline::Optimizations::Sharded::findSplitPoint() for details.
     */
    class SplittableDocumentSource {
    public:
        /** returns a source to be run on the shards.
         *  if NULL, don't run on shards
         */
        virtual intrusive_ptr<DocumentSource> getShardSource() = 0;

        /** returns a source that combines results from shards.
         *  if NULL, don't run on merger
         */
        virtual intrusive_ptr<DocumentSource> getMergeSource() = 0;
    protected:
        // It is invalid to delete through a SplittableDocumentSource-typed pointer.
        virtual ~SplittableDocumentSource() {}
    };


    /** This class marks DocumentSources which need mongod-specific functionality.
     *  It causes a MongodInterface to be injected when in a mongod and prevents mongos from
     *  merging pipelines containing this stage.
     */
    class DocumentSourceNeedsMongod {
    public:
        // Wraps mongod-specific functions to allow linking into mongos.
        class MongodInterface {
        public:
            virtual ~MongodInterface() {};

            virtual DBClientBase* directClient() = 0; // Always returns a DBDirectClient

            // Note that in some rare cases this could return a false negative but will never return
            // a false positive. This method will be fixed in the future once it becomes possible to
            // avoid false negatives.
            virtual bool isSharded(const NamespaceString& ns) = 0;

            virtual bool isCapped(const NamespaceString& ns) = 0;

            // Add new methods as needed.
        };

        void injectMongodInterface(boost::shared_ptr<MongodInterface> mongod) {
            _mongod = mongod;
        }

    protected:
        // It is invalid to delete through a DocumentSourceNeedsMongod-typed pointer.
        virtual ~DocumentSourceNeedsMongod() {}

        // Gives subclasses access to a MongodInterface implementation
        shared_ptr<MongodInterface> _mongod;
    };


    class DocumentSourceBsonArray :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual Value serialize(bool explain = false) const;
        virtual void setSource(DocumentSource *pSource);
        virtual bool isValidInitialSource() const { return true; }

        /**
          Create a document source based on a BSON array.

          This is usually put at the beginning of a chain of document sources
          in order to fetch data from the database.

          CAUTION:  the BSON is not read until the source is used.  Any
          elements that appear after these documents must not be read until
          this source is exhausted.

          @param array the BSON array to treat as a document source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSourceBsonArray> create(
            const BSONObj& array,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

    private:
        DocumentSourceBsonArray(
            const BSONObj& embeddedArray,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        BSONObj embeddedObject;
        BSONObjIterator arrayIterator;
    };

    
    class DocumentSourceCommandShards :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual Value serialize(bool explain = false) const;
        virtual void setSource(DocumentSource *pSource);
        virtual bool isValidInitialSource() const { return true; }

        /* convenient shorthand for a commonly used type */
        typedef vector<Strategy::CommandResult> ShardOutput;

        /** Returns the result arrays from shards using the 2.4 protocol.
         *  Call this instead of getNext() if you want access to the raw streams.
         *  This method should only be called at most once.
         */
        vector<BSONArray> getArrays();

        /**
          Create a DocumentSource that wraps the output of many shards

          @param shardOutput output from the individual shards
          @param pExpCtx the expression context for the pipeline
          @returns the newly created DocumentSource
         */
        static intrusive_ptr<DocumentSourceCommandShards> create(
            const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

    private:
        DocumentSourceCommandShards(const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

        /**
          Advance to the next document, setting pCurrent appropriately.

          Adjusts pCurrent, pBsonSource, and iterator, as needed.  On exit,
          pCurrent is the Document to return, or NULL.  If NULL, this
          indicates there is nothing more to return.
         */
        void getNextDocument();

        bool unstarted;
        bool hasCurrent;
        bool newSource; // set to true for the first item of a new source
        intrusive_ptr<DocumentSourceBsonArray> pBsonSource;
        Document pCurrent;
        ShardOutput::const_iterator iterator;
        ShardOutput::const_iterator listEnd;
    };


    /**
     * Constructs and returns Documents from the BSONObj objects produced by a supplied Cursor.
     * An object of this type may only be used by one thread, see SERVER-6123.
     */
    class DocumentSourceCursor :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCursor();
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual Value serialize(bool explain = false) const;
        virtual void setSource(DocumentSource *pSource);
        virtual bool coalesce(const intrusive_ptr<DocumentSource>& nextSource);
        virtual bool isValidInitialSource() const { return true; }
        virtual void dispose();

        /**
         * Create a document source based on a passed-in cursor.
         *
         * This is usually put at the beginning of a chain of document sources
         * in order to fetch data from the database.
         *
         * The DocumentSource takes ownership of the cursor and will destroy it
         * when the DocumentSource is finished with the cursor, if it hasn't
         * already been destroyed.
         *
         * @param ns the namespace the cursor is over
         * @param cursorId the id of the cursor to use
         * @param pExpCtx the expression context for the pipeline
         */
        static intrusive_ptr<DocumentSourceCursor> create(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Record the query that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the query to record
         */
        void setQuery(const BSONObj& query) { _query = query; }

        /*
          Record the sort that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the sort to record
         */
        void setSort(const BSONObj& sort) { _sort = sort; }

        /**
         * Informs this object of projection and dependency information.
         *
         * @param projection A projection specification describing the fields needed by the rest of
         *                   the pipeline.
         * @param deps The output of DocumentSource::parseDeps.
         * @param projectionInQuery True if the underlying cursor will handle the projection for us.
         */
        void setProjection(const BSONObj& projection,
                           const ParsedDeps& deps,
                           bool projectionInQuery);

        /// returns -1 for no limit
        long long getLimit() const;

    private:
        DocumentSourceCursor(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        void loadBatch();

        std::deque<Document> _currentBatch;

        // BSONObj members must outlive _projection and cursor.
        BSONObj _query;
        BSONObj _sort;
        BSONObj _projection;
        bool _haveDeps;
        ParsedDeps _dependencies;
        bool _projectionInQuery;
        intrusive_ptr<DocumentSourceLimit> _limit;
        long long _docsAddedToBatches; // for _limit enforcement

        string _ns; // namespace
        CursorId _cursorId;
    };


    class DocumentSourceGroup : public DocumentSource
                              , public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual void optimize();
        virtual GetDepsReturn getDependencies(set<string>& deps) const;
        virtual void dispose();
        virtual Value serialize(bool explain = false) const;

        /**
          Create a new grouping DocumentSource.
          
          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceGroup> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Set the Id Expression.

          Documents that pass through the grouping Document are grouped
          according to this key.  This will generate the id_ field in the
          result documents.

          @param pExpression the group key
         */
        void setIdExpression(const intrusive_ptr<Expression> &pExpression);

        /**
          Add an accumulator.

          Accumulators become fields in the Documents that result from
          grouping.  Each unique group document must have it's own
          accumulator; the accumulator factory is used to create that.

          @param fieldName the name the accumulator result will have in the
                result documents
          @param pAccumulatorFactory used to create the accumulator for the
                group field
         */
        void addAccumulator(const std::string& fieldName,
                            intrusive_ptr<Accumulator> (*pAccumulatorFactory)(),
                            const intrusive_ptr<Expression> &pExpression);

        /// Tell this source if it is doing a merge from shards. Defaults to false.
        void setDoingMerge(bool doingMerge) { _doingMerge = doingMerge; }

        /**
          Create a grouping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getMergeSource();

        static const char groupName[];

    private:
        DocumentSourceGroup(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /// Spill groups map to disk and returns an iterator to the file.
        shared_ptr<Sorter<Value, Value>::Iterator> spill();

        // Only used by spill. Would be function-local if that were legal in C++03.
        class SpillSTLComparator;

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;

        intrusive_ptr<Expression> pIdExpression;

        typedef vector<intrusive_ptr<Accumulator> > Accumulators;
        typedef boost::unordered_map<Value, Accumulators, Value::Hash> GroupsMap;
        GroupsMap groups;

        /*
          The field names for the result documents and the accumulator
          factories for the result documents.  The Expressions are the
          common expressions used by each instance of each accumulator
          in order to find the right-hand side of what gets added to the
          accumulator.  Note that each of those is the same for each group,
          so we can share them across all groups by adding them to the
          accumulators after we use the factories to make a new set of
          accumulators for each new group.

          These three vectors parallel each other.
        */
        vector<string> vFieldName;
        vector<intrusive_ptr<Accumulator> (*)()> vpAccumulatorFactory;
        vector<intrusive_ptr<Expression> > vpExpression;


        Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

        bool _doingMerge;
        bool _spilled;
        const bool _extSortAllowed;
        const int _maxMemoryUsageBytes;
        boost::scoped_ptr<Variables> _variables;

        // only used when !_spilled
        GroupsMap::iterator groupsIterator;

        // only used when _spilled
        scoped_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
        pair<Value, Value> _firstPartOfNextGroup;
        Value _currentId;
        Accumulators _currentAccumulators;
    };


    class DocumentSourceMatch : public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource>& nextSource);
        virtual Value serialize(bool explain = false) const;
        virtual void setSource(DocumentSource* Source);

        /**
          Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @returns the filter
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pCtx);

        /// Returns the query in Matcher syntax.
        BSONObj getQuery() const;

        static const char matchName[];

        /** Returns the portion of the match that can safely be promoted to before a $redact.
         *  If this returns an empty BSONObj, no part of this match may safely be promoted.
         *
         *  To be safe to promote, removing a field from a document to be matched must not cause
         *  that document to be accepted when it would otherwise be rejected. As an example,
         *  {name: {$ne: "bob smith"}} accepts documents without a name field, which means that
         *  running this filter before a redact that would remove the name field would leak
         *  information. On the other hand, {age: {$gt:5}} is ok because it doesn't accept documents
         *  that have had their age field removed.
         */
        BSONObj redactSafePortion() const;

        static bool isTextQuery(const BSONObj& query);
        bool isTextQuery() const { return _isTextQuery; }

    private:
        DocumentSourceMatch(const BSONObj &query,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        scoped_ptr<Matcher> matcher;
        bool _isTextQuery;
    };

    class DocumentSourceMergeCursors :
        public DocumentSource {
    public:
        typedef vector<pair<ConnectionString, CursorId> > CursorIds;

        // virtuals from DocumentSource
        boost::optional<Document> getNext();
        virtual void setSource(DocumentSource *pSource);
        virtual const char *getSourceName() const;
        virtual void dispose();
        virtual Value serialize(bool explain = false) const;
        virtual bool isValidInitialSource() const { return true; }

        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static intrusive_ptr<DocumentSource> create(
            const CursorIds& cursorIds,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char name[];

        /** Returns non-owning pointers to cursors managed by this stage.
         *  Call this instead of getNext() if you want access to the raw streams.
         *  This method should only be called at most once.
         */
        vector<DBClientCursor*> getCursors();

    private:

        struct CursorAndConnection {
            CursorAndConnection(ConnectionString host, NamespaceString ns, CursorId id);
            ScopedDbConnection connection;
            DBClientCursor cursor;
        };

        // using list to enable removing arbitrary elements
        typedef list<boost::shared_ptr<CursorAndConnection> > Cursors;

        DocumentSourceMergeCursors(
            const CursorIds& cursorIds,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Converts _cursorIds into active _cursors.
        void start();

        // This is the description of cursors to merge.
        const CursorIds _cursorIds;

        // These are the actual cursors we are merging. Created lazily.
        Cursors _cursors;
        Cursors::iterator _currentCursor;

        bool _unstarted;
    };

    class DocumentSourceOut : public DocumentSource
                            , public SplittableDocumentSource
                            , public DocumentSourceNeedsMongod {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceOut();
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual Value serialize(bool explain = false) const;

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource() { return NULL; }
        virtual intrusive_ptr<DocumentSource> getMergeSource() { return this; }

        const NamespaceString& getOutputNs() const { return _outputNs; }

        /**
          Create a document source for output and pass-through.

          This can be put anywhere in a pipeline and will store content as
          well as pass it on.

          @param pBsonElement the raw BSON specification for the source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char outName[];

    private:
        DocumentSourceOut(const NamespaceString& outputNs,
                          const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Sets _tempsNs and prepares it to receive data.
        void prepTempCollection();

        void spill(DBClientBase* conn, const vector<BSONObj>& toInsert);

        bool _done;

        NamespaceString _tempNs; // output goes here as it is being processed.
        const NamespaceString _outputNs; // output will go here after all data is processed.
    };

    
    class DocumentSourceProject :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual void optimize();
        virtual Value serialize(bool explain = false) const;

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char projectName[];

        /** projection as specified by the user */
        BSONObj getRaw() const { return _raw; }

    private:
        DocumentSourceProject(const intrusive_ptr<ExpressionContext>& pExpCtx,
                              const intrusive_ptr<ExpressionObject>& exprObj);

        // configuration state
        boost::scoped_ptr<Variables> _variables;
        intrusive_ptr<ExpressionObject> pEO;
        BSONObj _raw;

#if defined(_DEBUG)
        // this is used in DEBUG builds to ensure we are compatible
        Projection _simpleProjection;
#endif
    };

    class DocumentSourceRedact :
        public DocumentSource {
    public:
        virtual boost::optional<Document> getNext();
        virtual const char* getSourceName() const;
        virtual void optimize();

        static const char redactName[];

        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext>& expCtx);

        virtual Value serialize(bool explain = false) const;

    private:
        DocumentSourceRedact(const intrusive_ptr<ExpressionContext>& expCtx,
                             const intrusive_ptr<Expression>& previsit);

        // These both work over _variables
        boost::optional<Document> redactObject(); // redacts CURRENT
        Value redactValue(const Value& in);

        Variables::Id _currentId;
        boost::scoped_ptr<Variables> _variables;
        intrusive_ptr<Expression> _expression;
    };

    class DocumentSourceSort : public DocumentSource
                             , public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual void serializeToArray(vector<Value>& array, bool explain = false) const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual void dispose();

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getMergeSource();

        /**
          Add sort key field.

          Adds a sort key field to the key being built up.  A concatenated
          key is built up by calling this repeatedly.

          @param fieldPath the field path to the key component
          @param ascending if true, use the key for an ascending sort,
            otherwise, use it for descending
        */
        void addKey(const string &fieldPath, bool ascending);

        /// Write out a Document whose contents are the sort key.
        Document serializeSortKey(bool explain) const;

        /**
          Create a sorting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context for the pipeline
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /// Create a DocumentSourceSort with a given sort and (optional) limit
        static intrusive_ptr<DocumentSourceSort> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            BSONObj sortOrder,
            long long limit=-1);

        /// returns -1 for no limit
        long long getLimit() const;

        intrusive_ptr<DocumentSourceLimit> getLimitSrc() const { return limitSrc; }

        static const char sortName[];

    private:
        DocumentSourceSort(const intrusive_ptr<ExpressionContext> &pExpCtx);

        virtual Value serialize(bool explain = false) const {
            verify(false); // should call addToBsonArray instead
        }

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;

        SortOptions makeSortOptions() const;

        // These are used to merge pre-sorted results from a DocumentSourceMergeCursors or a
        // DocumentSourceCommandShards depending on whether we have finished upgrading to 2.6 or
        // not.
        class IteratorFromCursor;
        class IteratorFromBsonArray;
        void populateFromCursors(const vector<DBClientCursor*>& cursors);
        void populateFromBsonArrays(const vector<BSONArray>& arrays);

        /* these two parallel each other */
        typedef vector<intrusive_ptr<Expression> > SortKey;
        SortKey vSortKey;
        vector<char> vAscending; // used like vector<bool> but without specialization

        /// Extracts the fields in vSortKey from the Document;
        Value extractKey(const Document& d) const;

        /// Compare two Values according to the specified sort key.
        int compare(const Value& lhs, const Value& rhs) const;

        typedef Sorter<Value, Document> MySorter;

        // For MySorter
        class Comparator {
        public:
            explicit Comparator(const DocumentSourceSort& source): _source(source) {}
            int operator()(const MySorter::Data& lhs, const MySorter::Data& rhs) const {
                return _source.compare(lhs.first, rhs.first);
            }
        private:
            const DocumentSourceSort& _source;
        };

        intrusive_ptr<DocumentSourceLimit> limitSrc;

        bool _done;
        bool _mergingPresorted;
        scoped_ptr<MySorter::Iterator> _output;
    };

    class DocumentSourceLimit : public DocumentSource
                              , public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual Value serialize(bool explain = false) const;

        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return SEE_NEXT; // This doesn't affect needed fields
        }

        /**
          Create a new limiting DocumentSource.

          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceLimit> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            long long limit);

        // Virtuals for SplittableDocumentSource
        // Need to run on rounter. Running on shard as well is an optimization.
        virtual intrusive_ptr<DocumentSource> getShardSource() { return this; }
        virtual intrusive_ptr<DocumentSource> getMergeSource() { return this; }

        long long getLimit() const { return limit; }
        void setLimit(long long newLimit) { limit = newLimit; }

        /**
          Create a limiting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $limit.

          @param pBsonElement the BSONELement that defines the limit
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char limitName[];

    private:
        DocumentSourceLimit(const intrusive_ptr<ExpressionContext> &pExpCtx,
                            long long limit);

        long long limit;
        long long count;
    };

    class DocumentSourceSkip : public DocumentSource
                             , public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual Value serialize(bool explain = false) const;

        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return SEE_NEXT; // This doesn't affect needed fields
        }

        /**
          Create a new skipping DocumentSource.

          @param pExpCtx the expression context
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceSkip> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        // Need to run on rounter. Can't run on shards.
        virtual intrusive_ptr<DocumentSource> getShardSource() { return NULL; }
        virtual intrusive_ptr<DocumentSource> getMergeSource() { return this; }

        long long getSkip() const { return _skip; }
        void setSkip(long long newSkip) { _skip = newSkip; }

        /**
          Create a skipping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $skip.

          @param pBsonElement the BSONELement that defines the skip
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char skipName[];

    private:
        DocumentSourceSkip(const intrusive_ptr<ExpressionContext> &pExpCtx);

        long long _skip;
        bool _needToSkip;
    };


    class DocumentSourceUnwind :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual Value serialize(bool explain = false) const;

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char unwindName[];

    private:
        DocumentSourceUnwind(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /** Specify the field to unwind. */
        void unwindPath(const FieldPath &fieldPath);

        // Configuration state.
        scoped_ptr<FieldPath> _unwindPath;

        // Iteration state.
        class Unwinder;
        scoped_ptr<Unwinder> _unwinder;
    };

    class DocumentSourceGeoNear : public DocumentSource
                                , public SplittableDocumentSource
                                , public DocumentSourceNeedsMongod {
    public:
        // virtuals from DocumentSource
        virtual boost::optional<Document> getNext();
        virtual const char *getSourceName() const;
        virtual void setSource(DocumentSource *pSource);
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual bool isValidInitialSource() const { return true; }
        virtual Value serialize(bool explain = false) const;

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getMergeSource();

        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pCtx);

        static char geoNearName[];

        long long getLimit() { return limit; }

        // this should only be used for testing
        static intrusive_ptr<DocumentSourceGeoNear> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext> &pExpCtx);

        void parseOptions(BSONObj options);
        BSONObj buildGeoNearCmd() const;
        void runCommand();

        // These fields describe the command to run.
        // coords and distanceField are required, rest are optional
        BSONObj coords; // "near" option, but near is a reserved keyword on windows
        bool coordsIsArray;
        scoped_ptr<FieldPath> distanceField; // Using scoped_ptr because FieldPath can't be empty
        long long limit;
        double maxDistance;
        BSONObj query;
        bool spherical;
        double distanceMultiplier;
        scoped_ptr<FieldPath> includeLocs;
        bool uniqueDocs;

        // these fields are used while processing the results
        BSONObj cmdOutput;
        boost::scoped_ptr<BSONObjIterator> resultsIterator; // iterator over cmdOutput["results"]
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {
    inline void DocumentSourceGroup::setIdExpression(
        const intrusive_ptr<Expression> &pExpression) {
        pIdExpression = pExpression;
    }
}
