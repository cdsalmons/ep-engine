/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_DCP_PRODUCER_H_
#define SRC_DCP_PRODUCER_H_ 1

#include "config.h"

#include "dcp/stream.h"
#include "tapconnection.h"

class BackfillManager;
class DcpResponse;

class BufferLog {
public:
    BufferLog(uint32_t bytes)
        : max_bytes(bytes), bytes_sent(0) {}

    ~BufferLog() {}

    uint32_t getBufferSize() {
        return max_bytes;
    }

    void setBufferSize(uint32_t maxBytes) {
        max_bytes = maxBytes;
    }

    uint32_t getBytesSent() {
        return bytes_sent;
    }

    bool isFull() {
        return max_bytes <= bytes_sent;
    }

    void insert(DcpResponse* response);

    void free(uint32_t bytes_to_free);

private:
    uint32_t max_bytes;
    uint32_t bytes_sent;
};

class DcpProducer : public Producer {
public:

    DcpProducer(EventuallyPersistentEngine &e, const void *cookie,
                const std::string &n, bool notifyOnly);

    ~DcpProducer();

    ENGINE_ERROR_CODE streamRequest(uint32_t flags, uint32_t opaque,
                                    uint16_t vbucket, uint64_t start_seqno,
                                    uint64_t end_seqno, uint64_t vbucket_uuid,
                                    uint64_t last_seqno, uint64_t next_seqno,
                                    uint64_t *rollback_seqno,
                                    dcp_add_failover_log callback);

    ENGINE_ERROR_CODE getFailoverLog(uint32_t opaque, uint16_t vbucket,
                                     dcp_add_failover_log callback);

    ENGINE_ERROR_CODE step(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE bufferAcknowledgement(uint32_t opaque, uint16_t vbucket,
                                            uint32_t buffer_bytes);

    ENGINE_ERROR_CODE control(uint32_t opaque, const void* key, uint16_t nkey,
                              const void* value, uint32_t nvalue);

    ENGINE_ERROR_CODE handleResponse(protocol_binary_response_header *resp);

    void addStats(ADD_STAT add_stat, const void *c);

    void addTakeoverStats(ADD_STAT add_stat, const void* c, uint16_t vbid);

    // This function adds takeover (TO) stats and returns true if an entry
    // was found in the map that holds the vbucket information for streams
    // that were closed by the checkpoint remover's cursor dropped.
    bool addTOStatsIfStreamTempDisconnected(ADD_STAT add_stat, const void* c,
                                            uint16_t vbid);

    void aggregateQueueStats(ConnCounter* aggregator);

    void setDisconnect(bool disconnect);

    void notifySeqnoAvailable(uint16_t vbucket, uint64_t seqno);

    void vbucketStateChanged(uint16_t vbucket, vbucket_state_t state);

    bool closeSlowStream(uint16_t vbid, const std::string &name);

    void closeAllStreams();

    const char *getType() const;

    bool isTimeForNoop();

    void setTimeForNoop();

    void clearQueues();

    void appendQueue(std::list<queued_item> *q);

    size_t getBackfillQueueSize();

    size_t getItemsSent();

    size_t getTotalBytes();

    bool windowIsFull();

    void flush();

    std::list<uint16_t> getVBList(void);

    /**
     * Close the stream for given vbucket stream
     *
     * @param vbucket the if for the vbucket to close
     * @return ENGINE_SUCCESS upon a successful close
     *         ENGINE_NOT_MY_VBUCKET the vbucket stream doesn't exist
     */
    ENGINE_ERROR_CODE closeStream(uint32_t opaque, uint16_t vbucket);

    void notifyStreamReady(uint16_t vbucket, bool schedule);

    BackfillManager* getBackfillManager() {
        return backfillMgr;
    }

    bool isExtMetaDataEnabled () {
        return enableExtMetaData;
    }

    bool isValueCompressionEnabled() {
        return enableValueCompression;
    }

private:

    DcpResponse* getNextItem();

    size_t getItemsRemaining_UNLOCKED();

    ENGINE_ERROR_CODE maybeSendNoop(struct dcp_message_producers* producers);

    struct {
        rel_time_t sendTime;
        uint32_t opaque;
        uint32_t noopInterval;
        Couchbase::RelaxedAtomic<bool> pendingRecv;
        Couchbase::RelaxedAtomic<bool> enabled;
    } noopCtx;

    std::string priority;

    DcpResponse *rejectResp; // stash response for retry if E2BIG was hit

    bool notifyOnly;
    bool enableExtMetaData;

    bool enableValueCompression;

    bool supportsCursorDropping;

    Couchbase::RelaxedAtomic<rel_time_t> lastSendTime;
    BufferLog* log;
    BackfillManager* backfillMgr;
    std::list<uint16_t> ready;
    std::map<uint16_t, stream_t> streams;
    AtomicValue<size_t> itemsSent;
    AtomicValue<size_t> totalBytesSent;
    AtomicValue<size_t> ackedBytes;

    static const uint32_t defaultNoopInerval;

    /**
     * This map holds the vbucket id, and the last sent seqno
     * information for streams that have been dropped by the
     * checkpoint remover's cursor dropper, which are awaiting
     * reconnection.
     */
    std::map<uint16_t, uint64_t> tempDroppedStreams;
};

#endif  // SRC_DCP_PRODUCER_H_
