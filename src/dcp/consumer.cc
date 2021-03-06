/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "config.h"

#include "ep_engine.h"
#include "failover-table.h"
#include "connmap.h"
#include "replicationthrottle.h"
#include "dcp/consumer.h"
#include "dcp/stream.h"

const std::string DcpConsumer::noopCtrlMsg = "enable_noop";
const std::string DcpConsumer::noopIntervalCtrlMsg = "set_noop_interval";
const std::string DcpConsumer::connBufferCtrlMsg = "connection_buffer_size";
const std::string DcpConsumer::priorityCtrlMsg = "set_priority";
const std::string DcpConsumer::extMetadataCtrlMsg = "enable_ext_metadata";
const std::string DcpConsumer::valueCompressionCtrlMsg = "enable_value_compression";
const std::string DcpConsumer::cursorDroppingCtrlMsg = "supports_cursor_dropping";

class Processer : public GlobalTask {
public:
    Processer(EventuallyPersistentEngine* e, connection_t c,
                const Priority &p, double sleeptime = 1,
                bool completeBeforeShutdown = true)
        : GlobalTask(e, p, sleeptime, completeBeforeShutdown), conn(c) {}

    bool run();

    std::string getDescription();

    ~Processer();

private:
    connection_t conn;
};

bool Processer::run() {
    DcpConsumer* consumer = static_cast<DcpConsumer*>(conn.get());
    if (consumer->doDisconnect()) {
        return false;
    }

    switch (consumer->processBufferedItems()) {
        case all_processed:
            snooze(1);
            break;
        case more_to_process:
            snooze(0);
            break;
        case cannot_process:
            snooze(5);
            break;
        default:
            abort();
    }

    return true;
}

std::string Processer::getDescription() {
    std::stringstream ss;
    ss << "Processing buffered items for " << conn->getName();
    return ss.str();
}

Processer::~Processer() {
    DcpConsumer* consumer = static_cast<DcpConsumer*>(conn.get());
    consumer->taskCancelled();
}

DcpConsumer::DcpConsumer(EventuallyPersistentEngine &engine, const void *cookie,
                         const std::string &name)
    : Consumer(engine, cookie, name), opaqueCounter(0), processerTaskId(0),
      itemsToProcess(false), lastNoopTime(ep_current_time()), backoffs(0),
      taskAlreadyCancelled(false), flowControl(engine, this)
{
    Configuration& config = engine.getConfiguration();
    streams = new passive_stream_t[config.getMaxVbuckets()];
    setSupportAck(false);
    setLogHeader("DCP (Consumer) " + getName() + " -");
    setReserved(true);

    noopInterval = config.getDcpNoopInterval();

    pendingEnableNoop = config.isDcpEnableNoop();
    pendingSendNoopInterval = config.isDcpEnableNoop();
    pendingSetPriority = true;
    pendingEnableExtMetaData = true;
    pendingEnableValueCompression = config.isDcpValueCompressionEnabled();
    pendingSupportCursorDropping = true;

    ExTask task = new Processer(&engine, this, Priority::PendingOpsPriority, 1);
    processerTaskId = ExecutorPool::get()->schedule(task, NONIO_TASK_IDX);
}

DcpConsumer::~DcpConsumer() {
    cancelTask();
    closeAllStreams();

    delete[] streams;
}


void DcpConsumer::cancelTask() {
    bool inverse = false;
    if (taskAlreadyCancelled.compare_exchange_strong(inverse, true)) {
        ExecutorPool::get()->cancel(processerTaskId);
    }
}

void DcpConsumer::taskCancelled() {
    bool inverse = false;
    taskAlreadyCancelled.compare_exchange_strong(inverse, true);
}

ENGINE_ERROR_CODE DcpConsumer::addStream(uint32_t opaque, uint16_t vbucket,
                                         uint32_t flags) {
    LockHolder lh(readyMutex);
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    RCPtr<VBucket> vb = engine_.getVBucket(vbucket);
    if (!vb) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Add stream failed because this "
            "vbucket doesn't exist", logHeader(), vbucket);
        return ENGINE_NOT_MY_VBUCKET;
    }

    if (vb->getState() == vbucket_state_active) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Add stream failed because this "
            "vbucket happens to be in active state", logHeader(), vbucket);
        return ENGINE_NOT_MY_VBUCKET;
    }

    snapshot_info_t info = vb->checkpointManager.getSnapshotInfo();
    if (info.range.end == info.start) {
        info.range.start = info.start;
    }

    uint32_t new_opaque = ++opaqueCounter;
    failover_entry_t entry = vb->failovers->getLatestEntry();
    uint64_t start_seqno = info.start;
    uint64_t end_seqno = std::numeric_limits<uint64_t>::max();
    uint64_t vbucket_uuid = entry.vb_uuid;
    uint64_t snap_start_seqno = info.range.start;
    uint64_t snap_end_seqno = info.range.end;
    uint64_t high_seqno = vb->getHighSeqno();

    passive_stream_t stream = streams[vbucket];
    if (stream && stream->isActive()) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Cannot add stream because one "
            "already exists", logHeader(), vbucket);
        return ENGINE_KEY_EEXISTS;
    }

    streams[vbucket] = new PassiveStream(&engine_, this, getName(), flags,
                                         new_opaque, vbucket, start_seqno,
                                         end_seqno, vbucket_uuid,
                                         snap_start_seqno, snap_end_seqno,
                                         high_seqno);
    ready.push_back(vbucket);
    opaqueMap_[new_opaque] = std::make_pair(opaque, vbucket);

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::closeStream(uint32_t opaque, uint16_t vbucket) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    opaque_map::iterator oitr = opaqueMap_.find(opaque);
    if (oitr != opaqueMap_.end()) {
        opaqueMap_.erase(oitr);
    }

    passive_stream_t stream = streams[vbucket];
    if (!stream) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Cannot close stream because no "
            "stream exists for this vbucket", logHeader(), vbucket);
        return ENGINE_KEY_ENOENT;
    }

    uint32_t bytesCleared = stream->setDead(END_STREAM_CLOSED);
    flowControl.incrFreedBytes(bytesCleared);
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::streamEnd(uint32_t opaque, uint16_t vbucket,
                                         uint32_t flags) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    passive_stream_t stream = streams[vbucket];
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        LOG(EXTENSION_LOG_INFO, "%s (vb %d) End stream received with reason %d",
            logHeader(), vbucket, flags);

        StreamEndResponse* response;
        try {
            response = new StreamEndResponse(opaque, flags, vbucket);
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        err = stream->messageReceived(response);

        bool disable = false;
        if (err == ENGINE_TMPFAIL &&
            itemsToProcess.compare_exchange_strong(disable, true)) {
            ExecutorPool::get()->wake(processerTaskId);
        }
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        return ENGINE_SUCCESS;
    }

    if (err != ENGINE_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) End stream received with opaque "
            "%d but does not exist", logHeader(), vbucket, opaque);
    }

    flowControl.incrFreedBytes(StreamEndResponse::baseMsgBytes);
    return err;
}

ENGINE_ERROR_CODE DcpConsumer::mutation(uint32_t opaque, const void* key,
                                        uint16_t nkey, const void* value,
                                        uint32_t nvalue, uint64_t cas,
                                        uint16_t vbucket, uint32_t flags,
                                        uint8_t datatype, uint32_t locktime,
                                        uint64_t bySeqno, uint64_t revSeqno,
                                        uint32_t exptime, uint8_t nru,
                                        const void* meta, uint16_t nmeta) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (bySeqno == 0) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Invalid sequence number(0) "
            "for mutation!", logHeader(), vbucket);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    passive_stream_t stream = streams[vbucket];
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        Item *item = new Item(key, nkey, flags, exptime, value, nvalue,
                              &datatype, EXT_META_LEN, cas, bySeqno,
                              vbucket, revSeqno);

        ExtendedMetaData *emd = NULL;
        if (nmeta > 0) {
            emd = new ExtendedMetaData(meta, nmeta);

            if (emd == NULL) {
                return ENGINE_ENOMEM;
            }
            if (emd->getStatus() == ENGINE_EINVAL) {
                delete emd;
                return ENGINE_EINVAL;
            }
        }

        MutationResponse* response;
        try {
            response = new MutationResponse(item, opaque, emd);
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        err = stream->messageReceived(response);

        bool disable = false;
        if (err == ENGINE_TMPFAIL &&
            itemsToProcess.compare_exchange_strong(disable, true)) {
            ExecutorPool::get()->wake(processerTaskId);
        }
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        return ENGINE_SUCCESS;
    }

    uint32_t bytes =
        MutationResponse::mutationBaseMsgBytes + nkey + nmeta + nvalue;
    flowControl.incrFreedBytes(bytes);

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::deletion(uint32_t opaque, const void* key,
                                        uint16_t nkey, uint64_t cas,
                                        uint16_t vbucket, uint64_t bySeqno,
                                        uint64_t revSeqno, const void* meta,
                                        uint16_t nmeta) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (bySeqno == 0) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Invalid sequence number(0)"
            "for deletion!", logHeader(), vbucket);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    passive_stream_t stream = streams[vbucket];
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        Item* item = new Item(key, nkey, 0, 0, NULL, 0, NULL, 0, cas, bySeqno,
                              vbucket, revSeqno);
        item->setDeleted();

        ExtendedMetaData *emd = NULL;
        if (nmeta > 0) {
            emd = new ExtendedMetaData(meta, nmeta);

            if (emd == NULL) {
                return ENGINE_ENOMEM;
            }
            if (emd->getStatus() == ENGINE_EINVAL) {
                delete emd;
                return ENGINE_EINVAL;
            }
        }

        MutationResponse* response;
        try {
            response = new MutationResponse(item, opaque, emd);
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        err = stream->messageReceived(response);

        bool disable = false;
        if (err == ENGINE_TMPFAIL &&
            itemsToProcess.compare_exchange_strong(disable, true)) {
            ExecutorPool::get()->wake(processerTaskId);
        }
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        return ENGINE_SUCCESS;
    }

    uint32_t bytes = MutationResponse::deletionBaseMsgBytes + nkey + nmeta;
    flowControl.incrFreedBytes(bytes);

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::expiration(uint32_t opaque, const void* key,
                                          uint16_t nkey, uint64_t cas,
                                          uint16_t vbucket, uint64_t bySeqno,
                                          uint64_t revSeqno, const void* meta,
                                          uint16_t nmeta) {
    return deletion(opaque, key, nkey, cas, vbucket, bySeqno, revSeqno, meta,
                    nmeta);
}

ENGINE_ERROR_CODE DcpConsumer::snapshotMarker(uint32_t opaque,
                                              uint16_t vbucket,
                                              uint64_t start_seqno,
                                              uint64_t end_seqno,
                                              uint32_t flags) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (start_seqno > end_seqno) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Invalid snapshot marker "
            "received, snap_start (%" PRIu64 ") <= snap_end (%" PRIu64 ")",
            logHeader(), vbucket, start_seqno, end_seqno);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    passive_stream_t stream = streams[vbucket];
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        SnapshotMarker* response;
        try {
            response = new SnapshotMarker(opaque, vbucket, start_seqno,
                                          end_seqno, flags);
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        err = stream->messageReceived(response);

        bool disable = false;
        if (err == ENGINE_TMPFAIL &&
            itemsToProcess.compare_exchange_strong(disable, true)) {
            ExecutorPool::get()->wake(processerTaskId);
        }
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        return ENGINE_SUCCESS;
    }

    flowControl.incrFreedBytes(SnapshotMarker::baseMsgBytes);

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::noop(uint32_t opaque) {
    lastNoopTime = ep_current_time();
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::flush(uint32_t opaque, uint16_t vbucket) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    return ENGINE_ENOTSUP;
}

ENGINE_ERROR_CODE DcpConsumer::setVBucketState(uint32_t opaque,
                                               uint16_t vbucket,
                                               vbucket_state_t state) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    passive_stream_t stream = streams[vbucket];
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        SetVBucketState* response;
        try {
            response = new SetVBucketState(opaque, vbucket, state);
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        err = stream->messageReceived(response);

        bool disable = false;
        if (err == ENGINE_TMPFAIL &&
            itemsToProcess.compare_exchange_strong(disable, true)) {
            ExecutorPool::get()->wake(processerTaskId);
        }
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        return ENGINE_SUCCESS;
    }

    flowControl.incrFreedBytes(SetVBucketState::baseMsgBytes);

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::step(struct dcp_message_producers* producers) {
    setLastWalkTime();

    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    ENGINE_ERROR_CODE ret;
    if ((ret = flowControl.handleFlowCtl(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    if ((ret = handleNoop(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    if ((ret = handlePriority(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    if ((ret = handleExtMetaData(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    if ((ret = handleValueCompression(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    if ((ret = supportCursorDropping(producers)) != ENGINE_FAILED) {
        if (ret == ENGINE_SUCCESS) {
            ret = ENGINE_WANT_MORE;
        }
        return ret;
    }

    DcpResponse *resp = getNextItem();
    if (resp == NULL) {
        return ENGINE_SUCCESS;
    }

    ret = ENGINE_SUCCESS;
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    switch (resp->getEvent()) {
        case DCP_ADD_STREAM:
        {
            AddStreamResponse *as = static_cast<AddStreamResponse*>(resp);
            ret = producers->add_stream_rsp(getCookie(), as->getOpaque(),
                                            as->getStreamOpaque(),
                                            as->getStatus());
            break;
        }
        case DCP_STREAM_REQ:
        {
            StreamRequest *sr = static_cast<StreamRequest*> (resp);
            ret = producers->stream_req(getCookie(), sr->getOpaque(),
                                        sr->getVBucket(), sr->getFlags(),
                                        sr->getStartSeqno(), sr->getEndSeqno(),
                                        sr->getVBucketUUID(),
                                        sr->getSnapStartSeqno(),
                                        sr->getSnapEndSeqno());
            break;
        }
        case DCP_SET_VBUCKET:
        {
            SetVBucketStateResponse* vs;
            vs = static_cast<SetVBucketStateResponse*>(resp);
            ret = producers->set_vbucket_state_rsp(getCookie(), vs->getOpaque(),
                                                   vs->getStatus());
            break;
        }
        case DCP_SNAPSHOT_MARKER:
        {
            SnapshotMarkerResponse* mr;
            mr = static_cast<SnapshotMarkerResponse*>(resp);
            ret = producers->marker_rsp(getCookie(), mr->getOpaque(),
                                        mr->getStatus());
            break;
        }
        default:
            LOG(EXTENSION_LOG_WARNING, "%s Unknown consumer event (%d), "
                "disconnecting", logHeader(), resp->getEvent());
            ret = ENGINE_DISCONNECT;
    }
    ObjectRegistry::onSwitchThread(epe);
    delete resp;

    if (ret == ENGINE_SUCCESS) {
        return ENGINE_WANT_MORE;
    }
    return ret;
}

bool RollbackTask::run() {
    if (cons->doRollback(opaque, vbid, rollbackSeqno)) {
        return true;
    }
    ++(engine->getEpStats().rollbackCount);
    return false;
}

ENGINE_ERROR_CODE DcpConsumer::handleResponse(
                                        protocol_binary_response_header *resp) {
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    uint8_t opcode = resp->response.opcode;
    uint32_t opaque = resp->response.opaque;

    opaque_map::iterator oitr = opaqueMap_.find(opaque);

    bool validOpaque = false;
    if (oitr != opaqueMap_.end()) {
        validOpaque = isValidOpaque(opaque, oitr->second.second);
    }

    if (!validOpaque) {
        LOG(EXTENSION_LOG_WARNING, "%s Received response with opaque %"
            PRIu32 " and that stream no longer exists", logHeader(), opaque);
        return ENGINE_KEY_ENOENT;
    }

    if (opcode == PROTOCOL_BINARY_CMD_DCP_STREAM_REQ) {
        protocol_binary_response_dcp_stream_req* pkt =
            reinterpret_cast<protocol_binary_response_dcp_stream_req*>(resp);

        uint16_t vbid = oitr->second.second;
        uint16_t status = ntohs(pkt->message.header.response.status);
        uint64_t bodylen = ntohl(pkt->message.header.response.bodylen);
        uint8_t* body = pkt->bytes + sizeof(protocol_binary_response_header);

        if (status == PROTOCOL_BINARY_RESPONSE_ROLLBACK) {
            if (bodylen != sizeof(uint64_t)) {
                LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Received rollback "
                    "request with incorrect bodylen of %" PRIu64 ", disconnecting",
                    logHeader(), vbid, bodylen);
                return ENGINE_DISCONNECT;
            }
            uint64_t rollbackSeqno = 0;
            memcpy(&rollbackSeqno, body, sizeof(uint64_t));
            rollbackSeqno = ntohll(rollbackSeqno);

            LOG(EXTENSION_LOG_NOTICE, "%s (vb %d) Received rollback request "
                "to rollback seq no. %" PRIu64, logHeader(), vbid, rollbackSeqno);

            ExTask task = new RollbackTask(&engine_, opaque, vbid,
                                           rollbackSeqno, this,
                                           Priority::TapBgFetcherPriority);
            ExecutorPool::get()->schedule(task, WRITER_TASK_IDX);
            return ENGINE_SUCCESS;
        }

        if (((bodylen % 16) != 0 || bodylen == 0) && status == ENGINE_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "%s (vb %d)Got a stream response with a "
                "bad failover log (length %" PRIu64 "), disconnecting",
                logHeader(), vbid, bodylen);
            return ENGINE_DISCONNECT;
        }

        streamAccepted(opaque, status, body, bodylen);
        return ENGINE_SUCCESS;
    } else if (opcode == PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT ||
               opcode == PROTOCOL_BINARY_CMD_DCP_CONTROL) {
        return ENGINE_SUCCESS;
    }

    LOG(EXTENSION_LOG_WARNING, "%s Trying to handle an unknown response %d, "
        "disconnecting", logHeader(), opcode);

    return ENGINE_DISCONNECT;
}

bool DcpConsumer::doRollback(uint32_t opaque, uint16_t vbid,
                             uint64_t rollbackSeqno) {
    ENGINE_ERROR_CODE err = engine_.getEpStore()->rollback(vbid, rollbackSeqno);

    switch (err) {
    case ENGINE_NOT_MY_VBUCKET:
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Rollback failed because the "
                "vbucket was not found", logHeader(), vbid);
        return false;

    case ENGINE_TMPFAIL:
        return true; // Reschedule the rollback.

    case ENGINE_SUCCESS:
        // expected
        break;

    default:
        throw std::logic_error("DcpConsumer::doRollback: Unexpected error "
                "code from EpStore::rollback: " + std::to_string(err));
    }

    RCPtr<VBucket> vb = engine_.getVBucket(vbid);
    streams[vbid]->reconnectStream(vb, opaque, vb->getHighSeqno());

    return false;
}

bool DcpConsumer::reconnectSlowStream(StreamEndResponse *resp) {
    /**
     * To be invoked only if END_STREAM was received, and the reconnection
     * is initiated only if the reason states SLOW.
     */
    if (resp == nullptr) {
        throw std::invalid_argument("DcpConsumer::reconnectSlowStream: resp is NULL");
    }

    if (resp->getFlags() == END_STREAM_SLOW) {
        uint16_t vbid = resp->getVbucket();
        RCPtr<VBucket> vb = engine_.getVBucket(vbid);
        if (vb) {
            passive_stream_t stream = streams[vbid];
            if (stream) {
                LOG(EXTENSION_LOG_NOTICE, "%s (vb %d) Consumer is attempting "
                        "to reconnect stream, as it received END_STREAM for "
                        "the vbucket with reason as SLOW", logHeader(), vbid);
                stream->reconnectStream(vb, resp->getOpaque(),
                                        vb->getHighSeqno());
                return true;
            }
        }
    }
    return false;
}

void DcpConsumer::addStats(ADD_STAT add_stat, const void *c) {
    ConnHandler::addStats(add_stat, c);

    int max_vbuckets = engine_.getConfiguration().getMaxVbuckets();
    for (int vbucket = 0; vbucket < max_vbuckets; vbucket++) {
        passive_stream_t stream = streams[vbucket];
        if (stream) {
            stream->addStats(add_stat, c);
        }
    }

    addStat("total_backoffs", backoffs, add_stat, c);
    flowControl.addStats(add_stat, c);
}

void DcpConsumer::aggregateQueueStats(ConnCounter* aggregator) {
    aggregator->conn_queueBackoff += backoffs;
}

process_items_error_t DcpConsumer::processBufferedItems() {
    itemsToProcess.store(false);
    process_items_error_t process_ret = all_processed;

    int max_vbuckets = engine_.getConfiguration().getMaxVbuckets();
    for (int vbucket = 0; vbucket < max_vbuckets; vbucket++) {

        passive_stream_t stream = streams[vbucket];
        if (!stream) {
            continue;
        }

        uint32_t bytes_processed;

        do {
            if (!engine_.getReplicationThrottle().shouldProcess()) {
                backoffs++;
                return cannot_process;
            }

            bytes_processed = 0;
            process_ret = stream->processBufferedMessages(bytes_processed);
            flowControl.incrFreedBytes(bytes_processed);
        } while (bytes_processed > 0 && process_ret != cannot_process);
    }

    if (flowControl.isBufferSufficientlyDrained()) {
        /* Notify memcached to get flow control buffer ack out. We cannot wait
           till the ConnManager daemon task notifies the memcached as it would
           cause delay in buffer ack being sent out to the producer */
        engine_.getDcpConnMap().notifyPausedConnection(this, false);
    }

    if (process_ret == all_processed && itemsToProcess.load()) {
        return more_to_process;
    }

    return process_ret;
}

DcpResponse* DcpConsumer::getNextItem() {
    LockHolder lh(readyMutex);

    setPaused(false);
    while (!ready.empty()) {
        uint16_t vbucket = ready.front();
        ready.pop_front();

        passive_stream_t stream = streams[vbucket];
        if (!stream) {
            continue;
        }

        DcpResponse* op = stream->next();
        if (!op) {
            continue;
        }
        switch (op->getEvent()) {
            case DCP_STREAM_REQ:
            case DCP_ADD_STREAM:
            case DCP_SET_VBUCKET:
            case DCP_SNAPSHOT_MARKER:
                break;
            default:
                LOG(EXTENSION_LOG_WARNING, "%s Consumer is attempting to write"
                    " an unexpected event %d", logHeader(), op->getEvent());
                abort();
        }

        ready.push_back(vbucket);
        return op;
    }
    setPaused(true);

    return NULL;
}

void DcpConsumer::notifyStreamReady(uint16_t vbucket) {
    LockHolder lh(readyMutex);
    std::list<uint16_t>::iterator iter =
        std::find(ready.begin(), ready.end(), vbucket);
    if (iter != ready.end()) {
        return;
    }

    ready.push_back(vbucket);
    lh.unlock();

    engine_.getDcpConnMap().notifyPausedConnection(this, true);
}

void DcpConsumer::streamAccepted(uint32_t opaque, uint16_t status, uint8_t* body,
                                 uint32_t bodylen) {

    opaque_map::iterator oitr = opaqueMap_.find(opaque);
    if (oitr != opaqueMap_.end()) {
        uint32_t add_opaque = oitr->second.first;
        uint16_t vbucket = oitr->second.second;

        passive_stream_t stream = streams[vbucket];
        if (stream && stream->getOpaque() == opaque &&
            stream->getState() == STREAM_PENDING) {
            if (status == ENGINE_SUCCESS) {
                RCPtr<VBucket> vb = engine_.getVBucket(vbucket);
                vb->failovers->replaceFailoverLog(body, bodylen);
                EventuallyPersistentStore* st = engine_.getEpStore();
                st->scheduleVBSnapshot(Priority::VBucketPersistHighPriority,
                                st->getVBuckets().getShardByVbId(vbucket)->getId());
            }
            LOG(EXTENSION_LOG_INFO, "%s (vb %d) Add stream for opaque %" PRIu32
                " %s with error code %d", logHeader(), vbucket, opaque,
                status == ENGINE_SUCCESS ? "succeeded" : "failed", status);
            stream->acceptStream(status, add_opaque);
        } else {
            LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Trying to add stream, but "
                "none exists (opaque: %" PRIu32 ", add_opaque: %" PRIu32 ")",
                logHeader(), vbucket, opaque, add_opaque);
        }
        opaqueMap_.erase(opaque);
    } else {
        LOG(EXTENSION_LOG_WARNING, "%s No opaque found for add stream response "
            "with opaque %" PRIu32, logHeader(), opaque);
    }
}

bool DcpConsumer::isValidOpaque(uint32_t opaque, uint16_t vbucket) {
    passive_stream_t stream = streams[vbucket];
    return stream && stream->getOpaque() == opaque;
}

void DcpConsumer::closeAllStreams() {
    int max_vbuckets = engine_.getConfiguration().getMaxVbuckets();
    for (int vbucket = 0; vbucket < max_vbuckets; vbucket++) {
        passive_stream_t stream = streams[vbucket];
        if (stream) {
            stream->setDead(END_STREAM_DISCONNECTED);
        }
    }
}

ENGINE_ERROR_CODE DcpConsumer::handleNoop(struct dcp_message_producers* producers) {
    if (pendingEnableNoop) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 noopCtrlMsg.c_str(), noopCtrlMsg.size(),
                                 val.c_str(), val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingEnableNoop = false;
        return ret;
    }

    if (pendingSendNoopInterval) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        char buf_size[10];
        snprintf(buf_size, 10, "%u", noopInterval);
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 noopIntervalCtrlMsg.c_str(),
                                 noopIntervalCtrlMsg.size(),
                                 buf_size, strlen(buf_size));
        ObjectRegistry::onSwitchThread(epe);
        pendingSendNoopInterval = false;
        return ret;
    }

    if ((ep_current_time() - lastNoopTime) > (noopInterval * 2)) {
        LOG(EXTENSION_LOG_WARNING, "%s Disconnecting because noop message has "
            "not been received for %u seconds", logHeader(), (noopInterval * 2));
        return ENGINE_DISCONNECT;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handlePriority(struct dcp_message_producers* producers) {
    if (pendingSetPriority) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("high");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 priorityCtrlMsg.c_str(), priorityCtrlMsg.size(),
                                 val.c_str(), val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSetPriority = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handleExtMetaData(struct dcp_message_producers* producers) {
    if (pendingEnableExtMetaData) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 extMetadataCtrlMsg.c_str(),
                                 extMetadataCtrlMsg.size(),
                                 val.c_str(), val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingEnableExtMetaData = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handleValueCompression(struct dcp_message_producers* producers) {
    if (pendingEnableValueCompression) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 valueCompressionCtrlMsg.c_str(),
                                 valueCompressionCtrlMsg.size(),
                                 val.c_str(), val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingEnableValueCompression = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::supportCursorDropping(struct dcp_message_producers* producers) {
    if (pendingSupportCursorDropping) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(getCookie(), opaque,
                                 cursorDroppingCtrlMsg.c_str(),
                                 cursorDroppingCtrlMsg.size(),
                                 val.c_str(), val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSupportCursorDropping = false;
        return ret;
    }

    return ENGINE_FAILED;
}

uint64_t DcpConsumer::incrOpaqueCounter()
{
    return (++opaqueCounter);
}

uint32_t DcpConsumer::getFlowControlBufSize()
{
    return flowControl.getFlowControlBufSize();
}

void DcpConsumer::setFlowControlBufSize(uint32_t newSize)
{
    flowControl.setFlowControlBufSize(newSize);
}

const std::string& DcpConsumer::getControlMsgKey(void)
{
    return connBufferCtrlMsg;
}

bool DcpConsumer::isStreamPresent(uint16_t vbucket)
{
    if (streams[vbucket] && streams[vbucket]->isActive()) {
        return true;
    }
    return false;
}
