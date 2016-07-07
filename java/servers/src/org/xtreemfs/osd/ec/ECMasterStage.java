/*
 * Copyright (c) 2016 by Johannes Dillmann,
 *               Zuse Institute Berlin
 *
 * Licensed under the BSD License, see LICENSE file for details.
 *
 */

package org.xtreemfs.osd.ec;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

import org.xtreemfs.common.uuids.ServiceUUID;
import org.xtreemfs.common.uuids.UnknownUUIDException;
import org.xtreemfs.common.xloc.StripingPolicyImpl;
import org.xtreemfs.common.xloc.XLocations;
import org.xtreemfs.foundation.SSLOptions;
import org.xtreemfs.foundation.buffer.ASCIIString;
import org.xtreemfs.foundation.buffer.BufferPool;
import org.xtreemfs.foundation.buffer.ReusableBuffer;
import org.xtreemfs.foundation.flease.Flease;
import org.xtreemfs.foundation.flease.FleaseStage;
import org.xtreemfs.foundation.flease.FleaseStatusListener;
import org.xtreemfs.foundation.flease.FleaseViewChangeListenerInterface;
import org.xtreemfs.foundation.flease.comm.FleaseMessage;
import org.xtreemfs.foundation.flease.proposer.FleaseException;
import org.xtreemfs.foundation.intervals.AVLTreeIntervalVector;
import org.xtreemfs.foundation.intervals.Interval;
import org.xtreemfs.foundation.intervals.IntervalVector;
import org.xtreemfs.foundation.logging.Logging;
import org.xtreemfs.foundation.logging.Logging.Category;
import org.xtreemfs.foundation.pbrpc.client.RPCAuthentication;
import org.xtreemfs.foundation.pbrpc.client.RPCNIOSocketClient;
import org.xtreemfs.foundation.pbrpc.client.RPCResponse;
import org.xtreemfs.foundation.pbrpc.client.RPCResponseAvailableListener;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.ErrorType;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.POSIXErrno;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.RPCHeader.ErrorResponse;
import org.xtreemfs.foundation.pbrpc.utils.ErrorUtils;
import org.xtreemfs.osd.FleasePrefixHandler;
import org.xtreemfs.osd.OSDRequest;
import org.xtreemfs.osd.OSDRequestDispatcher;
import org.xtreemfs.osd.ec.ECFileState.FileState;
import org.xtreemfs.osd.ec.ECMasterStage.ResponseResultManager.ResponseResult;
import org.xtreemfs.osd.ec.ECMasterStage.ResponseResultManager.ResponseResultListener;
import org.xtreemfs.osd.stages.Stage;
import org.xtreemfs.osd.stages.StorageStage.GetECVectorsCallback;
import org.xtreemfs.pbrpc.generatedinterfaces.GlobalTypes.FileCredentials;
import org.xtreemfs.pbrpc.generatedinterfaces.GlobalTypes.StripingPolicyType;
import org.xtreemfs.pbrpc.generatedinterfaces.OSD.xtreemfs_ec_get_interval_vectorsResponse;
import org.xtreemfs.pbrpc.generatedinterfaces.OSDServiceClient;

import com.google.protobuf.Message;

public class ECMasterStage extends Stage {
    private static final int               MAX_EXTERNAL_REQUESTS_IN_Q = 250;
    private static final int               MAX_PENDING_PER_FILE       = 10;

    private static final String            FLEASE_PREFIX              = "/ec/";

    private final ServiceUUID              localUUID;
    private final ASCIIString              localID;

    private final OSDRequestDispatcher     master;

    private final FleaseStage              fstage;

    private final RPCNIOSocketClient       client;

    private final OSDServiceClient         osdClient;

    private final AtomicInteger            externalRequestsInQueue;

    private final Map<String, ECFileState> fileStates;

    public ECMasterStage(OSDRequestDispatcher master, SSLOptions sslOpts, int maxRequestsQueueLength,
            FleaseStage fstage, FleasePrefixHandler fleaseHandler) throws IOException {
        super("ECMasterStage", maxRequestsQueueLength);
        this.master = master;

        // FIXME (jdillmann): Do i need my own RPC client? What should be the timeouts?
        client = new RPCNIOSocketClient(sslOpts, 15000, 60000 * 5, "ECMasterStage");
        osdClient = new OSDServiceClient(client, null);
        externalRequestsInQueue = new AtomicInteger(0);

        fileStates = new HashMap<String, ECFileState>();

        // TODO (jdillmann): make local id a parameter?
        localUUID = master.getConfig().getUUID();
        localID = new ASCIIString(localUUID.toString());

        this.fstage = fstage;
        master.getFleaseHandler().registerPrefix(FLEASE_PREFIX, new FleaseViewChangeListenerInterface() {

            @Override
            public void viewIdChangeEvent(ASCIIString cellId, int viewId, boolean onProposal) {
                // eventViewIdChanged(cellId, viewId, onProposal);
                // FIXME (jdillmann): Implement views on EC
            }
        }, new FleaseStatusListener() {

            @Override
            public void statusChanged(ASCIIString cellId, Flease lease) {
                eventLeaseStateChanged(cellId, lease, null);
            }

            @Override
            public void leaseFailed(ASCIIString cellId, FleaseException error) {
                // change state
                // flush pending requests
                eventLeaseStateChanged(cellId, null, error);
            }
        });

    }

    @Override
    public void start() {
        client.start();
        super.start();
    }

    @Override
    public void shutdown() {
        client.shutdown();
        super.shutdown();
    }

    @Override
    public void waitForStartup() throws Exception {
        client.waitForStartup();
        super.waitForStartup();
    }

    @Override
    public void waitForShutdown() throws Exception {
        client.waitForShutdown();
        super.waitForShutdown();
    }

    void enqueueOperation(final STAGE_OP stageOp, final Object[] args, final OSDRequest request,
            final ReusableBuffer createdViewBuffer, final Object callback) {
        enqueueOperation(stageOp.ordinal(), args, request, createdViewBuffer, callback);
    }

    void enqueueExternalOperation(final STAGE_OP stageOp, final Object[] args, final OSDRequest request,
            final ReusableBuffer createdViewBuffer, final Object callback) {
        if (externalRequestsInQueue.get() >= MAX_EXTERNAL_REQUESTS_IN_Q) {
            Logging.logMessage(Logging.LEVEL_WARN, this, "EC stage is overloaded, request %d for %s dropped",
                    request.getRequestId(), request.getFileId());
            request.sendInternalServerError(
                    new IllegalStateException("EC replication stage is overloaded, request dropped"));

            // Make sure that the data buffer is returned to the pool if
            // necessary, as some operations create view buffers on the
            // data. Otherwise, a 'finalized but not freed before' warning
            // may occur.
            if (createdViewBuffer != null) {
                assert (createdViewBuffer.getRefCount() >= 2);
                BufferPool.free(createdViewBuffer);
            }

        } else {
            externalRequestsInQueue.incrementAndGet();
            this.enqueueOperation(stageOp, args, request, createdViewBuffer, callback);
        }
    }

    void enqueuePrioritized(StageRequest rq) {
        while (!q.offer(rq)) {
            StageRequest otherRq = q.poll();
            otherRq.sendInternalServerError(
                    new IllegalStateException("internal queue overflow, cannot enqueue operation for processing."));
            Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this,
                    "Dropping request from rwre queue due to overload");
        }
    }

    private static enum STAGE_OP {
        PREPARE, READ, WRITE, TRUNCATE, LEASE_STATE_CHANGED, LOCAL_VECTORS_AVAILABLE, REMOTE_VECTORS_AVAILABLE;

        private static STAGE_OP[] values_ = values();

        public static STAGE_OP valueOf(int n) {
            return values_[n];
        }
    };

    @Override
    protected void processMethod(StageRequest method) {
        switch (STAGE_OP.valueOf(method.getStageMethod())) {
        // External requests
        case PREPARE:
            externalRequestsInQueue.decrementAndGet();
            processPrepare(method);
            break;
        case READ:
            externalRequestsInQueue.decrementAndGet();
            break;
        case WRITE:
            externalRequestsInQueue.decrementAndGet();
            break;
        case TRUNCATE:
            externalRequestsInQueue.decrementAndGet();
            break;
        // Internal requests
        case LEASE_STATE_CHANGED:
            processLeaseChanged(method);
            break;
        case LOCAL_VECTORS_AVAILABLE:
            processLocalVectorsAvailable(method);
            break;
        case REMOTE_VECTORS_AVAILABLE:
            processRemoteVectorsAvailable(method);
            break;

        // default : throw new IllegalArgumentException("No such stageop");
        }
    }

    /**
     * If the file is already open, return the cached fileState. <br>
     * If it is closed create a FileState in state INITALIZING and open a flease cell.
     */
    ECFileState getFileState(FileCredentials credentials, XLocations locations, boolean invalidated) {

        final String fileId = credentials.getXcap().getFileId();
        ECFileState file = fileStates.get(fileId);

        if (file == null) {
            Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this, "Opening FileState for: %s", fileId);

            ASCIIString cellId = new ASCIIString(FLEASE_PREFIX + fileId);

            file = new ECFileState(fileId, cellId, localUUID, locations, credentials);
            fileStates.put(fileId, file);
        }

        if (file.getState() == FileState.INITIALIZING && !file.isCellOpen()) {
            doWaitingForLease(file);
        }

        return file;
    }

    /**
     * This will reset the FileState to INITIALIZING and aborts pending requests.<br>
     * If a flease cell is open, it will be closed
     */
    void failed(final ECFileState file, final ErrorResponse ex) {
        // FIXME (jdillmann): Ensure everything is reset to the INIT state
        Logging.logMessage(Logging.LEVEL_WARN, Category.ec, this,
                "(R:%s) replica for file %s failed (in state: %s): %s", localID, file.getFileId(), file.getState(),
                ErrorUtils.formatError(ex));
        file.resetDefaults();
        fstage.closeCell(file.getCellId(), false);
        abortPendingRequests(file, ex);
    }

    /**
     * Abort all pending requests associated with this FileState.
     */
    void abortPendingRequests(final ECFileState file, ErrorResponse error) {
        if (error == null) {
            error = ErrorUtils.getErrorResponse(ErrorType.INTERNAL_SERVER_ERROR, POSIXErrno.POSIX_ERROR_NONE,
                    "Request had been aborted.");
        }

        while (file.hasPendingRequests()) {
            StageRequest request = file.removePendingRequest();
            Object callback = request.getCallback();
            if (callback != null && callback instanceof FallibleCallback) {
                ((FallibleCallback) callback).failed(error);
            }
        }
    }

    /**
     * Go to state WAITING_FOR_LEASE and open the flease cell. <br>
     * When flease is ready {@link #eventLeaseStateChanged} will be called.
     */
    void doWaitingForLease(final ECFileState file) {
        assert (!file.isCellOpen());
        // FIXME (jdillmann): Maybe check if cell is already open
        // can't happen right now, since doWaitingForLease is only called from getState

        try {
            XLocations locations = file.getLocations();
            List<ServiceUUID> remoteOSDs = file.getRemoteOSDs();
            List<InetSocketAddress> acceptors = new ArrayList<InetSocketAddress>(remoteOSDs.size());
            for (ServiceUUID service : remoteOSDs) {
                acceptors.add(service.getAddress());
            }

            fstage.openCell(file.getCellId(), acceptors, true, locations.getVersion());

            file.setState(FileState.WAITING_FOR_LEASE);
            file.setCellOpen(true);

        } catch (UnknownUUIDException ex) {
            failed(file, ErrorUtils.getErrorResponse(ErrorType.ERRNO, POSIXErrno.POSIX_ERROR_EIO, ex.toString(), ex));
        }

    }

    void doPrimary(final ECFileState file) {
        file.setState(FileState.PRIMARY);

        while (file.hasPendingRequests()) {
            enqueuePrioritized(file.removePendingRequest());
        }
    }

    /**
     * Go to state BACKUP and re-enqueue pending requests prioritized.
     */
    void doBackup(final ECFileState file) {
        file.setState(FileState.BACKUP);

        while (file.hasPendingRequests()) {
            enqueuePrioritized(file.removePendingRequest());
        }
    }

    /**
     * Go to state VERSION_RESET and start to fetch local and remote versions.<br>
     * When enough versions are fetched {@link #eventVersionsFetched} will be called.
     */
    void doVersionReset(final ECFileState file) {
        file.setState(FileState.VERSION_RESET);
        fetchLocalVectors(file);
    }

    /**
     * This will be called every time the flease cell changed.
     */
    void eventLeaseStateChanged(ASCIIString cellId, Flease lease, FleaseException error) {
        this.enqueueOperation(STAGE_OP.LEASE_STATE_CHANGED, new Object[] { cellId, lease, error }, null, null, null);
    }

    void processLeaseChanged(StageRequest method) {
        final ASCIIString cellId = (ASCIIString) method.getArgs()[0];
        final Flease lease = (Flease) method.getArgs()[1];
        final FleaseException error = (FleaseException) method.getArgs()[2];

        final String fileId = FleasePrefixHandler.stripPrefix(cellId).toString();
        final ECFileState file = fileStates.get(fileId);

        if (file == null) {
            // Lease cells can be opened from any OSD in the Xloc, thus the file has not to be opened on this OSD.
            // Receiving an leaseChangeEvent is no error.
            Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this,
                    "Received LeaseChange event for non opened file (%s)", fileId);
            return;
        }

        if (error != null) {
            failed(file, ErrorUtils.getInternalServerError(error, "Lease Error"));
            return;
        }

        // TODO(jdillmann): Invalidation stuff?
        FileState state = file.getState();
        boolean localIsPrimary = (lease.getLeaseHolder() != null) && (lease.getLeaseHolder().equals(localID));
        file.setLocalIsPrimary(localIsPrimary);
        file.setLease(lease);

        // Handle timeouts on the primary by throwing an error
        if (state == FileState.PRIMARY && lease.getLeaseHolder() == null && lease.getLeaseTimeout_ms() == 0) {
            Logging.logMessage(Logging.LEVEL_ERROR, Category.ec, this,
                    "(R:%s) was primary, lease error in cell %s, restarting replication: %s", localID, cellId, lease);
            failed(file,
                    ErrorUtils.getInternalServerError(new IOException(fileId + ": lease timed out, renew failed")));
            return;
        }

        // The VERSION_RESET is only triggered if the OSD became primary. If in the meantime it was deselected the
        // operation should be aborted
        if (state == FileState.VERSION_RESET && !localIsPrimary) {
            Logging.logMessage(Logging.LEVEL_ERROR, Category.ec, this,
                    "(R:%s) is in VERSION_RESET but lost its PRIMARY state in cell %s: %s", localID, cellId, lease);
            failed(file,
                    ErrorUtils.getInternalServerError(new IOException("Primary was deselected during VERSION_RESET")));
            return;
        }

        // FIXME (jdillmann): Check again which states will result in an error

        // Only make a transition if the file is in a valid state.
        if (state == FileState.PRIMARY || state == FileState.BACKUP || state == FileState.WAITING_FOR_LEASE) {
            if (localIsPrimary && state != FileState.PRIMARY) {
                // The local OSD became primary and has been BACKUP or WAITING before
                file.setMasterEpoch(lease.getMasterEpochNumber());
                doVersionReset(file);

            } else if (!localIsPrimary && state != FileState.BACKUP) {
                // The local OSD became backup and has been PRIMARY or WAITING before
                file.setMasterEpoch(FleaseMessage.IGNORE_MASTER_EPOCH);
                doBackup(file);
            }
        }
    }

    public void prepare(FileCredentials credentials, XLocations xloc, Interval interval, PrepareCallback callback,
            OSDRequest request) {
        this.enqueueExternalOperation(STAGE_OP.PREPARE, new Object[] { credentials, xloc, interval }, request, null,
                callback);
    }

    public static interface PrepareCallback extends FallibleCallback {
        public void success(final List<Interval> curVersions, final List<Interval> nextVersions);

        public void redirect(String redirectTo);
    }

    void processPrepare(StageRequest method) {
        final PrepareCallback callback = (PrepareCallback) method.getCallback();
        final FileCredentials credentials = (FileCredentials) method.getArgs()[0];
        final XLocations loc = (XLocations) method.getArgs()[1];
        final Interval interval = (Interval) method.getArgs()[2];

        final String fileId = credentials.getXcap().getFileId();
        StripingPolicyImpl sp = loc.getLocalReplica().getStripingPolicy();
        assert (sp.getPolicy().getType() == StripingPolicyType.STRIPING_POLICY_ERASURECODE);

        final ECFileState file = getFileState(credentials, loc, false);
        // FIXME (jdillmann): Deadlock, falls bei doWaitingForLease Fehler auftreten.
        // Dann bleibt der Zustand INIT, aber es wird nicht nochmal doWaiting aufgerufen
        // Fehler auch bei RWR vorhanden, daher erstmal ignorieren

        // if (!isInternal)
        file.setCredentials(credentials);

        switch (file.getState()) {

        case INITIALIZING:
        case WAITING_FOR_LEASE:
        case VERSION_RESET:
            if (file.sizeOfPendingRequests() > MAX_PENDING_PER_FILE) {
                if (Logging.isDebug()) {
                    Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this,
                            "Rejecting request: too many requests (is: %d, max %d) in queue for file %s",
                            file.sizeOfPendingRequests(), MAX_PENDING_PER_FILE, fileId);
                }
                callback.failed(ErrorUtils.getErrorResponse(ErrorType.INTERNAL_SERVER_ERROR,
                        POSIXErrno.POSIX_ERROR_NONE, "too many requests in queue for file"));
            } else {
                file.addPendingRequest(method);
            }
            return;

        case BACKUP:
            assert (!file.isLocalIsPrimary());
            // if (!isInternal)

            Flease lease = file.getLease();
            if (lease.isEmptyLease()) {
                Logging.logMessage(Logging.LEVEL_WARN, Category.replication, this, "Unknown lease state for %s: %s",
                        file.getCellId(), lease);
                ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.INTERNAL_SERVER_ERROR,
                        POSIXErrno.POSIX_ERROR_EAGAIN, "Unknown lease state for cell " + file.getCellId()
                                + ", can't redirect to master. Please retry.");
                // FIXME (jdillmann): abort all requests?
                callback.failed(error);
                return;
            }

            callback.redirect(lease.getLeaseHolder().toString());
            return;

        case PRIMARY:
            assert (file.isLocalIsPrimary());
            // FIXME (jdillmann): Return real version vectors
            callback.success(null, null);
            return;

        // case INVALIDATED:
        // break;
        // case OPENING:
        // break;

        // default:
        // break;

        }

    }

    /**
     * This will request the local and remote VersionTrees. On success eventVersionResetComplete is called
     * 
     * @param file
     */
    // TODO (jdillmann): Think about moving this to a separate class or OSD Event Method
    void fetchLocalVectors(final ECFileState file) {
        final String fileId = file.getFileId();
        master.getStorageStage().getECVectors(fileId, null, new GetECVectorsCallback() {
            @Override
            public void getECVectorsComplete(IntervalVector curVector, IntervalVector nextVector, ErrorResponse error) {
                eventLocalVectorsAvailable(fileId, curVector, nextVector, error);
            }
        });
    }

    void eventLocalVectorsAvailable(String fileId, IntervalVector curVector, IntervalVector nextVector,
            ErrorResponse error) {
        this.enqueueOperation(STAGE_OP.LOCAL_VECTORS_AVAILABLE, new Object[] { fileId, curVector, nextVector, error },
                null, null, null);
    }

    void processLocalVectorsAvailable(StageRequest method) {
        final String fileId = (String) method.getArgs()[0];
        final IntervalVector curVector = (IntervalVector) method.getArgs()[1];
        final IntervalVector nextVector = (IntervalVector) method.getArgs()[2];
        final ErrorResponse error = (ErrorResponse) method.getArgs()[3];

        final ECFileState file = fileStates.get(fileId);

        if (file == null) {
            Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this,
                    "Received LocalIntervalVectorAvailable event for non opened file (%s)", fileId);
            return;
        }

        if (error != null) {
            failed(file, error);
            return;
        }

        assert (file.state == FileState.VERSION_RESET);
        fetchRemoteVersions(file, curVector, nextVector);
    }

    /**
     * This will be called after the local VersionTree has been fetched.
     * 
     * @param file
     * @param localCurVector
     * @param nextCurVector
     */
    // TODO (jdillmann): Think about moving this to a separate class or OSD Event Method
    void fetchRemoteVersions(final ECFileState file, final IntervalVector localCurVector,
            final IntervalVector localNextVector) {

        // TODO (jdillmann): Or only an if
        assert (file.state == FileState.VERSION_RESET);

        final String fileId = file.getFileId();
        
        List<ServiceUUID> remoteUUIDs = file.getRemoteOSDs();
        int numRemotes = remoteUUIDs.size();

        ResponseResultListener<xtreemfs_ec_get_interval_vectorsResponse, Integer> listener;
        listener = new ResponseResultListener<xtreemfs_ec_get_interval_vectorsResponse, Integer>() {

            @Override
            public void success(ResponseResult<xtreemfs_ec_get_interval_vectorsResponse, Integer>[] results) {
                eventRemoteVectorsAvailable(fileId, localCurVector, localNextVector, results, null);
            }

            @Override
            public void failed(ResponseResult<xtreemfs_ec_get_interval_vectorsResponse, Integer>[] results) {
                // TODO (jdillmann): Add numberOfFailures as a parameter?
                String errorMsg = String.format("(EC: %s) VectorReset failed due to too many unreachable remote OSDs.",
                        localUUID);
                ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.IO_ERROR, POSIXErrno.POSIX_ERROR_EIO, errorMsg);
                eventRemoteVectorsAvailable(fileId, null, null, null, error);
            }
        };

        ResponseResultManager<xtreemfs_ec_get_interval_vectorsResponse, Integer> manager;
        manager = new ResponseResultManager<xtreemfs_ec_get_interval_vectorsResponse, Integer>(numRemotes, listener);

        try {
            for (int i = 0; i < numRemotes; i++) {
                ServiceUUID uuid = remoteUUIDs.get(i);

                RPCResponse<xtreemfs_ec_get_interval_vectorsResponse> response;
                response = osdClient.xtreemfs_ec_get_interval_vectors(uuid.getAddress(), RPCAuthentication.authNone,
                        RPCAuthentication.userService, file.getCredentials(), fileId);
                manager.add(response, i);
            }
        } catch (IOException ex) {
            failed(file, ErrorUtils.getInternalServerError(ex));
        }
    }

    void eventRemoteVectorsAvailable(String fileId, IntervalVector localCurVector, IntervalVector localNextVector,
            ResponseResult<xtreemfs_ec_get_interval_vectorsResponse, Integer>[] results, ErrorResponse error) {
        this.enqueueOperation(STAGE_OP.REMOTE_VECTORS_AVAILABLE,
                new Object[] { fileId, localCurVector, localNextVector, results, error }, null, null, null);
    }

    void processRemoteVectorsAvailable(StageRequest method) {
        final String fileId = (String) method.getArgs()[0];
        final IntervalVector localCurVector = (IntervalVector) method.getArgs()[1];
        final IntervalVector localNextVector = (IntervalVector) method.getArgs()[2];
        @SuppressWarnings("unchecked")
        final ResponseResult<xtreemfs_ec_get_interval_vectorsResponse, Integer>[] results = 
            (ResponseResult<xtreemfs_ec_get_interval_vectorsResponse, Integer>[]) method.getArgs()[3];
        final ErrorResponse error = (ErrorResponse) method.getArgs()[4];

        final ECFileState file = fileStates.get(fileId);

        if (file == null) {
            Logging.logMessage(Logging.LEVEL_DEBUG, Category.ec, this,
                    "Received LocalIntervalVectorAvailable event for non opened file (%s)", fileId);
            return;
        }

        if (error != null) {
            failed(file, error);
            return;
        }

        assert (file.state == FileState.VERSION_RESET);
        
        IntervalVector[] curVectors = new IntervalVector[results.length + 1];
        IntervalVector[] nextVectors = new IntervalVector[results.length + 1];
        
        // Store local vectors at the end
        curVectors[results.length] = localCurVector;
        nextVectors[results.length] = localNextVector;

        int responseCount = 0;

        // Transform protobuf messages to IntervalVectors
        for (int r = 0; r < results.length; r++) {
            xtreemfs_ec_get_interval_vectorsResponse response = results[r].getResult();
            
            if (response != null) {
                responseCount++;

                curVectors[r] = new AVLTreeIntervalVector();
                for (int i = 0; i < response.getCurIntervalsCount(); i++) {
                    curVectors[r].insert(new ProtoInterval(response.getCurIntervals(i)));
                }

                nextVectors[r] = new AVLTreeIntervalVector();
                for (int i = 0; i < response.getNextIntervalsCount(); i++) {
                    nextVectors[r].insert(new ProtoInterval(response.getNextIntervals(i)));
                }
            } else {
                curVectors[r] = null;
                nextVectors[r] = null;
            }
        }

        // Recover the latest interval vectors from the available vectors.
        final AVLTreeIntervalVector resultVector = new AVLTreeIntervalVector();
        boolean needsCommit;
        try {
            needsCommit = file.getPolicy().recoverVector(responseCount, curVectors, nextVectors, resultVector);
        } catch (Exception e) {
            // FIXME (jdillmann): Do something
            Logging.logError(Logging.LEVEL_WARN, this, e);
            failed(file, null);
            return;
        }

        if (needsCommit) {
            // Well well... and we have to do another round!
            // FIXME (jdillmann): Not implemented yet
            throw new RuntimeException("Not implemented yet");

        } else {
            // FIXME (jdillmann): Maybe send async commit
            file.setCurVector(resultVector);
            doPrimary(file);
        }
    }


    static class ResponseResultManager<M extends Message, O> implements RPCResponseAvailableListener<M> {
        final AtomicInteger                count;
        final RPCResponse<M>[]             responses;
        final ResponseResult<M, O>[]       results;

        final int                          numAcksRequired;
        final ResponseResultListener<M, O> listener;

        final AtomicInteger                numQuickFail;
        final AtomicInteger                numResponses;
        final AtomicInteger                numErrors;

        public ResponseResultManager(int capacity, ResponseResultListener<M, O> listener) {
            this(capacity, capacity, listener);
        }

        @SuppressWarnings("unchecked")
        public ResponseResultManager(int capacity, int numAcksRequired, ResponseResultListener<M, O> listener) {
            count = new AtomicInteger(0);
            numQuickFail = new AtomicInteger(0);
            numResponses = new AtomicInteger(0);
            numErrors = new AtomicInteger(0);

            responses = new RPCResponse[capacity];
            results = new ResponseResult[capacity];

            this.numAcksRequired = numAcksRequired;
            this.listener = listener;
        }

        public void add(RPCResponse<M> response, O object) {
            add(response, object, false);
        }

        public void add(RPCResponse<M> response, O object, boolean quickFail) {
            int i = count.getAndIncrement();
            if (i >= responses.length) {
                throw new IndexOutOfBoundsException();
            }

            if (quickFail) {
                numQuickFail.incrementAndGet();
            }

            responses[i] = response;
            results[i] = new ResponseResult<M, O>(object, quickFail);

            response.registerListener(this);
        }

        @SuppressWarnings("rawtypes")
        static int indexOf(RPCResponse[] responses, RPCResponse response) {
            for (int i = 0; i < responses.length; ++i) {
                if (responses[i].equals(response)) {
                    return i;
                }
            }
            return -1;
        }

        @Override
        public void responseAvailable(RPCResponse<M> r) {
            int i = indexOf(responses, r);
            if (i < 0) {
                Logging.logMessage(Logging.LEVEL_WARN, Category.ec, this, "received unknown response");
                r.freeBuffers();
                return;
            }

            ResponseResult<M, O> responseResult = results[i];

            int curNumResponses, curNumErrors, curNumQuickFail;

            // Decrement the number of outstanding requests that may fail quick.
            if (responseResult.mayQuickFail()) {
                curNumQuickFail = numQuickFail.decrementAndGet();
            } else {
                curNumQuickFail = numQuickFail.get();
            }

            try {
                // Try to get and add the result.
                M result = r.get();
                responseResult.setResult(result);
                curNumResponses = numResponses.incrementAndGet();
                curNumErrors = numErrors.get();

            } catch (Exception ec) {
                // Try to
                responseResult.setFailed();
                curNumErrors = numErrors.incrementAndGet();
                curNumResponses = numResponses.get();

            } finally {
                r.freeBuffers();
            }

            // TODO(jdillmann): Think about waiting for quickFail timeouts if numAcksReq is not fullfilled otherwise.
            if (curNumResponses + curNumErrors + curNumQuickFail == responses.length) {
                if (curNumResponses >= numAcksRequired) {
                    listener.success(results);
                } else {
                    listener.failed(results);
                }
            }
        }

        public static class ResponseResult<M extends Message, O> {
            private final O       object;
            private final boolean quickFail;
            private boolean       failed;
            private M             result;

            ResponseResult(O object, boolean quickFail) {
                this.failed = false;
                this.result = null;
                this.object = object;
                this.quickFail = quickFail;
            }

            synchronized void setFailed() {
                this.failed = true;
                this.result = null;
            }

            synchronized public boolean hasFailed() {
                return failed;
            }

            synchronized public boolean hasFinished() {
                return (result != null || failed);
            }

            synchronized void setResult(M result) {
                this.failed = false;
                this.result = result;
            }

            synchronized public M getResult() {
                return result;
            }

            public O getMappedObject() {
                return object;
            }

            public boolean mayQuickFail() {
                return quickFail;
            }
        }

        public static interface ResponseResultListener<M extends Message, O> {
            void success(ResponseResult<M, O>[] results);

            void failed(ResponseResult<M, O>[] results);
        }

    }

}
