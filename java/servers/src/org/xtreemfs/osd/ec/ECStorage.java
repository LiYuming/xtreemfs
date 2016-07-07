/*
 * Copyright (c) 2016 by Johannes Dillmann, Zuse Institute Berlin
 *
 * Licensed under the BSD License, see LICENSE file for details.
 *
 */
package org.xtreemfs.osd.ec;

import java.io.IOException;
import java.util.Arrays;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;

import org.xtreemfs.common.libxtreemfs.exceptions.XtreemFSException;
import org.xtreemfs.common.xloc.StripingPolicyImpl;
import org.xtreemfs.foundation.buffer.BufferPool;
import org.xtreemfs.foundation.buffer.ReusableBuffer;
import org.xtreemfs.foundation.intervals.AVLTreeIntervalVector;
import org.xtreemfs.foundation.intervals.Interval;
import org.xtreemfs.foundation.intervals.IntervalVector;
import org.xtreemfs.foundation.intervals.ObjectInterval;
import org.xtreemfs.foundation.logging.Logging;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.ErrorType;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.POSIXErrno;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.RPCHeader.ErrorResponse;
import org.xtreemfs.foundation.pbrpc.utils.ErrorUtils;
import org.xtreemfs.osd.OSDRequestDispatcher;
import org.xtreemfs.osd.stages.Stage.StageRequest;
import org.xtreemfs.osd.stages.StorageStage.ECCommitVectorCallback;
import org.xtreemfs.osd.stages.StorageStage.ECGetVectorsCallback;
import org.xtreemfs.osd.stages.StorageStage.ECReadDataCallback;
import org.xtreemfs.osd.stages.StorageStage.ECWriteDataCallback;
import org.xtreemfs.osd.storage.FileMetadata;
import org.xtreemfs.osd.storage.MetadataCache;
import org.xtreemfs.osd.storage.ObjectInformation;
import org.xtreemfs.osd.storage.ObjectInformation.ObjectStatus;
import org.xtreemfs.osd.storage.StorageLayout;

/**
 * This class contains the methods regarding EC data and IntervalVector handling. <br>
 * For sake of clarity the methods are separated to this class. <br>
 * Since the IntervalVectors are tightly coupled to the data integrity they have to be handled in the same stage to
 * reduce the chance of failures and inconsistencies.<br>
 * Unfortunately this means, that the possibly expensive IntervalVector calculations and the expensive encoding
 * operations are also run in the StorageStage. If profiling shows their impact those methods should be moved to a
 * separate stage.
 */
public class ECStorage {
    private final MetadataCache        cache;
    private final StorageLayout        layout;
    private final OSDRequestDispatcher master;
    private final boolean              checksumsEnabled;

    public ECStorage(OSDRequestDispatcher master, MetadataCache cache, StorageLayout layout, boolean checksumsEnabled) {
        this.master = master;
        this.cache = cache;
        this.layout = layout;
        this.checksumsEnabled = checksumsEnabled;
    }

    public void processGetVectors(final StageRequest rq) {
        final ECGetVectorsCallback callback = (ECGetVectorsCallback) rq.getCallback();
        final String fileId = (String) rq.getArgs()[0];

        FileMetadata fi = cache.getFileInfo(fileId);
        if (fi == null) {
            try {
                IntervalVector curVector = new AVLTreeIntervalVector();
                layout.getECIntervalVector(fileId, false, curVector);

                IntervalVector nextVector = new AVLTreeIntervalVector();
                layout.getECIntervalVector(fileId, true, nextVector);

                callback.ecGetVectorsComplete(curVector, nextVector, null);

            } catch (Exception ex) {
                callback.ecGetVectorsComplete(null, null,
                        ErrorUtils.getErrorResponse(ErrorType.ERRNO, POSIXErrno.POSIX_ERROR_EIO, ex.toString()));
            }
        } else {
            callback.ecGetVectorsComplete(fi.getECCurVector(), fi.getECNextVector(), null);
        }
    }

    public void processCommitVector(final StageRequest rq) {
        final ECCommitVectorCallback callback = (ECCommitVectorCallback) rq.getCallback();
        final String fileId = (String) rq.getArgs()[0];
        final StripingPolicyImpl sp = (StripingPolicyImpl) rq.getArgs()[1];
        final List<Interval> commitIntervals = (List<Interval>) rq.getArgs()[2];

        try {
            final FileMetadata fi = layout.getFileMetadata(sp, fileId);

            List<Interval> toCommit = new LinkedList<Interval>();
            List<Interval> toAbort = new LinkedList<Interval>();

            if (!commitIntervals.isEmpty()) {
                boolean failed = calculateIntervalsToCommitAbort(commitIntervals, null, fi.getECCurVector().serialize(),
                        fi.getECNextVector().serialize(), toCommit, toAbort);

                if (failed) {
                    callback.ecCommitVectorComplete(true, null);
                    return;
                }

                for (Interval interval : toCommit) {
                    commitECData(fileId, fi, interval);
                }
            }

            // Abort everything in the next vector
            // FIXME (jdillmann): Also delete the EC data?
            AVLTreeIntervalVector emptyNextVector = new AVLTreeIntervalVector();
            layout.setECIntervalVector(fileId, emptyNextVector.serialize(), true, false);
            fi.setECNextVector(emptyNextVector);

            // FIXME (jdillmann): truncate cur?

            callback.ecCommitVectorComplete(false, null);

        } catch (IOException ex) {
            ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.ERRNO, POSIXErrno.POSIX_ERROR_EIO,
                    ex.toString(), ex);
            callback.ecCommitVectorComplete(false, error);
            return;
        }

    }

    public void processWriteData(final StageRequest rq) {
        final ECWriteDataCallback callback = (ECWriteDataCallback) rq.getCallback();
        final String fileId = (String) rq.getArgs()[0];
        final StripingPolicyImpl sp = (StripingPolicyImpl) rq.getArgs()[1];
        final long objectNo = (Long) rq.getArgs()[2];
        final int offset = (Integer) rq.getArgs()[3];
        final Interval reqInterval = (Interval) rq.getArgs()[4];
        final List<Interval> commitIntervals = (List<Interval>) rq.getArgs()[5];
        final ReusableBuffer data = (ReusableBuffer) rq.getArgs()[6];

        assert (!commitIntervals.isEmpty());

        boolean consistent = true;
        try {
            final FileMetadata fi = layout.getFileMetadata(sp, fileId);
            IntervalVector curVector = fi.getECCurVector();
            IntervalVector nextVector = fi.getECNextVector();

            long opStart = reqInterval.getOpStart();
            long opEnd = reqInterval.getOpEnd();

            // FIXME(jdillmann): A single write may never cross stripe boundaries

            // Get this nodes interval vectors and check if the vector this operation is based on can be fully committed
            long commitStart = commitIntervals.get(0).getOpStart();
            long commitEnd = commitIntervals.get(commitIntervals.size() - 1).getOpEnd();
            // FIXME (jdillmann): Check if this is correct
            List<Interval> curVecIntervals = curVector.getOverlapping(commitStart, commitEnd);
            List<Interval> nextVecIntervals = nextVector.getOverlapping(commitStart, commitEnd);

            LinkedList<Interval> toCommitAcc = new LinkedList<Interval>();
            LinkedList<Interval> toAbortAcc = new LinkedList<Interval>();

            boolean failed = calculateIntervalsToCommitAbort(commitIntervals, reqInterval, curVecIntervals,
                    nextVecIntervals, toCommitAcc, toAbortAcc);

            // Signal to go to reconstruct if the vector can not be fully committed.
            if (failed) {
                callback.ecWriteDataComplete(null, true, null);
                return;
            }

            // Commit or abort intervals from the next vector.
            for (Interval interval : toCommitAcc) {
                commitECData(fileId, fi, interval);
            }
            for (Interval interval : toAbortAcc) {
                abortECData(fileId, fi, interval);
            }

            if (data != null) {
                // Check operation boundaries.
                // FIXME (jdillmann): Check for exclusive ends
                long dataStart = sp.getObjectStartOffset(objectNo) + offset;
                long dataEnd = dataStart + data.capacity();
                // The data range has to be within the request interval and may not cross chunk boundaries
                assert (dataStart >= reqInterval.getStart() && dataEnd <= reqInterval.getEnd());
                assert (dataEnd - dataStart <= sp.getStripeSizeForObject(objectNo));

                // Write the data
                String fileIdNext = fileId + ".next";
                long version = 1;
                // FIXME (jdillmann): Decide if/when sync should be used
                boolean sync = false;
                layout.writeObject(fileIdNext, fi, data.createViewBuffer(), objectNo, offset, version, sync, false);
                consistent = false;

                // Store the vector
                layout.setECIntervalVector(fileId, Arrays.asList(reqInterval), true, true);
                fi.getECNextVector().insert(reqInterval);
                consistent = true;

                // Generate diff between the current data and the newly written data.
                byte[] diff = new byte[data.capacity()];
                ObjectInformation objInfo = layout.readObject(fileId, fi, objectNo, offset, data.capacity(), version);
                if (objInfo.getStatus() == ObjectStatus.PADDING_OBJECT
                        || objInfo.getStatus() == ObjectStatus.DOES_NOT_EXIST || objInfo.getData() == null) {
                    // There is no current data: the new data is the diff.
                    data.position(0);
                    data.get(diff);
                } else {
                    ReusableBuffer curData = objInfo.getData();
                    assert (curData.capacity() == data.capacity());

                    // byte-wise diff between cur and next data
                    // TODO (jdillmann): Benchmark if getting the buffers as byte[] would be faster
                    data.position(0);
                    curData.position(0);
                    for (int i = 0; i < data.capacity(); i++) {
                        diff[i] = (byte) (data.get() ^ curData.get());
                    }

                    BufferPool.free(curData);
                }

                // Return the diff buffer to the caller
                ReusableBuffer diffBuffer = ReusableBuffer.wrap(diff);
                callback.ecWriteDataComplete(diffBuffer, false, null);

            } else {
                // Store the vector
                layout.setECIntervalVector(fileId, Arrays.asList(reqInterval), true, true);
                fi.getECNextVector().insert(reqInterval);

                callback.ecWriteDataComplete(null, false, null);
            }

        } catch (IOException ex) {
            if (!consistent) {
                // FIXME (jdillmann): Inform in detail about critical error
                Logging.logError(Logging.LEVEL_CRIT, this, ex);
            }

            ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.ERRNO, POSIXErrno.POSIX_ERROR_EIO,
                    ex.toString(), ex);
            callback.ecWriteDataComplete(null, false, error);
        } finally {
            BufferPool.free(data);
        }
    }

    public void processReadData(final StageRequest rq) {
        final ECReadDataCallback callback = (ECReadDataCallback) rq.getCallback();
        final String fileId = (String) rq.getArgs()[0];
        final StripingPolicyImpl sp = (StripingPolicyImpl) rq.getArgs()[1];
        final long objNo = (Long) rq.getArgs()[2];
        final int offset = (Integer) rq.getArgs()[3];
        final int length = (Integer) rq.getArgs()[4];
        final List<Interval> intervals = (List<Interval>) rq.getArgs()[5];

        assert (!intervals.isEmpty());
        assert (offset + length <= sp.getStripeSizeForObject(objNo));

        final String fileIdNext = fileId + ".next";
        long objOffset = sp.getObjectStartOffset(objNo);

        // ReusableBuffer data = BufferPool.allocate(length);
        
        try {
            final FileMetadata fi = layout.getFileMetadata(sp, fileId);
            IntervalVector curVector = fi.getECCurVector();
            IntervalVector nextVector = fi.getECNextVector();

            long commitStart = intervals.get(0).getStart();
            long commitEnd = intervals.get(intervals.size() - 1).getEnd();

            List<Interval> curOverlapping = curVector.getOverlapping(commitStart, commitEnd);
            List<Interval> nextOverlapping = nextVector.getOverlapping(commitStart, commitEnd);

            LinkedList<Interval> toCommitAcc = new LinkedList<Interval>();
            LinkedList<Interval> toAbortAcc = new LinkedList<Interval>();
            boolean failed = calculateIntervalsToCommitAbort(intervals, null, curOverlapping, nextOverlapping,
                    toCommitAcc, toAbortAcc);

            // Signal to go to reconstruct if the vector can not be fully committed.
            // Note: this is actually to rigorous, since we would only have to care about the overlapping intervals with
            // the current object read range. But to keep it simple and uniform we require the whole commit interval to
            // be present. This (faulty) behavior is also present in the commit vector routines.
            if (failed) {
                callback.ecReadDataComplete(null, true, null);
                return;
            }

            // Commit or abort intervals from the next vector.
            for (Interval interval : toCommitAcc) {
                commitECData(fileId, fi, interval);
            }
            for (Interval interval : toAbortAcc) {
                abortECData(fileId, fi, interval);
            }

            int version = 1;
            ObjectInformation result = layout.readObject(fileId, fi, objNo, offset, length, version);
            // TODO (jdillmann): Add Metadata?
            // result.setChecksumInvalidOnOSD(checksumInvalidOnOSD);
            // result.setLastLocalObjectNo(lastLocalObjectNo);
            // result.setGlobalLastObjectNo(globalLastObjectNo);
            callback.ecReadDataComplete(result, false, null);


            // FIXME (jdillmann): Won't return the correct error if the read is outdated.

            //
            // Interval emptyInterval = ObjectInterval.empty(commitStart, commitEnd);
            //
            // Iterator<Interval> curIt = curOverlapping.iterator();
            // Iterator<Interval> nextIt = nextOverlapping.iterator();
            // Interval curInterval = curIt.hasNext() ? curIt.next() : emptyInterval;
            // Interval nextInterval = nextIt.hasNext() ? nextIt.next() : emptyInterval;
            //
            //
            // for (Interval interval : intervals) {
            // int iOffset = ECHelper.safeLongToInt(interval.getStart() - objOffset);
            // int iLength = ECHelper.safeLongToInt(interval.getEnd() - interval.getStart());
            // int version = 1;
            //
            // // Advance to the next interval that could be a possible match
            // // or set an empty interval as a placeholder
            // while (curInterval.getEnd() <= interval.getStart() && curIt.hasNext()) {
            // curInterval = curIt.next();
            // }
            // if (curInterval.getEnd() <= interval.getStart()) {
            // curInterval = emptyInterval;
            // }
            //
            // // Advance to the next interval that could be a possible match
            // // or set an empty interval as a placeholder
            // while (nextInterval.getEnd() <= interval.getStart() && nextIt.hasNext()) {
            // nextInterval = nextIt.next();
            // }
            // if (nextInterval.getEnd() <= interval.getStart()) {
            // nextInterval = emptyInterval;
            // }
            //
            // // equals: start, end, version, id
            // // Note: not opStart/opEnd
            // if (interval.equals(curInterval)) {
            // // FINE: do something
            // readToBuf(fileId, fi, objNo, iOffset, iLength, version, data);
            //
            // } else {
            // if (interval.overlaps(curInterval)
            // && interval.getVersion() < curInterval.getVersion()) {
            // // ERROR => non fatal, read request has to be retried or just abort
            // String errorMsg = String
            // .format("Could not read fileId '%s' due to a concurrent write operation.", fileId);
            // ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.ERRNO,
            // POSIXErrno.POSIX_ERROR_EAGAIN, errorMsg);
            // callback.ecReadDataComplete(null, false, error);
            // return;
            // }
            //
            // if (interval.equals(nextInterval)) {
            // // FINE
            // readToBuf(fileIdNext, fi, objNo, iOffset, iLength, version, data);
            // } else {
            // // NOT FOUND! => reconstruction required
            // callback.ecReadDataComplete(null, true, null);
            // return;
            // }
            // }
            // }
            //
            // data.position(0);
            // ObjectInformation result = new ObjectInformation(ObjectStatus.EXISTS, data, 0);
            // // TODO (jdillmann): Add Metadata?
            // // result.setChecksumInvalidOnOSD(checksumInvalidOnOSD);
            // // result.setLastLocalObjectNo(lastLocalObjectNo);
            // // result.setGlobalLastObjectNo(globalLastObjectNo);
            // callback.ecReadDataComplete(result, false, null);

        } catch (IOException ex) {
            // BufferPool.free(data);
            ErrorResponse error = ErrorUtils.getErrorResponse(ErrorType.ERRNO, POSIXErrno.POSIX_ERROR_EIO,
                    ex.toString(), ex);
            callback.ecReadDataComplete(null, false, error);
        }
        
    }

    // void readToBuf(String fileId, FileMetadata fi, long objNo, int offset, int length, int version, ReusableBuffer
    // data)
    // throws IOException {
    //
    // // TODO (jdillmann): Add read to existing buffer method to StorageLayout
    // ObjectInformation objInfo = layout.readObject(fileId, fi, objNo, offset, length, version);
    //
    // data.position(offset);
    // assert (data.remaining() >= length);
    //
    // if (objInfo.getStatus() == ObjectStatus.EXISTS) {
    // ReusableBuffer readBuf = objInfo.getData();
    // readBuf.position(0);
    // data.put(readBuf);
    // BufferPool.free(readBuf);
    // }
    //
    // // Fill with zeros
    // int remaining = (offset + length) - data.position();
    // for (int i = 0; i < remaining; i++) {
    // data.put((byte) 0);
    // }
    // }

    /**
     * Checks for each interval in commitIntervals if it is present in the curVecIntervals or the nextVecIntervals.<br>
     * If it is present in curVecIntervals, overlapping intervals from nextVecIntervals will be added to toAbortAcc.<br>
     * If it is present in nextVecIntervals, it is added to the toCommitAcc accumulator.<br>
     * If it isn't present in neither, false is returned immediately.<br>
     * If it's version is lower then an overlapping one from curVecIntervals, an exception is thrown.
     * 
     * @param commitIntervals
     *            List of intervals to commit. Not necessarily over the range of the whole file.
     * @param reqInterval
     *            The interval currently processed. Every interval in nextVecIntervals with the same version and op id
     *            will be ignored (neither committed or aborted). May be null.
     * @param curVecIntervals
     *            List of intervals from the currently stored data. Maybe sliced to the commitIntervals range.
     * @param nextVecIntervals
     *            List of intervals from the next buffer. Maybe sliced to the commitIntervals range.
     * @param toCommitAcc
     *            Accumulator used to return the intervals to be committed from the next buffer.
     * @param toAbortAcc
     *            Accumulator used to return the intervals to be aborted from the next buffer.
     * @return true if an interval from commitIntervals can not be found. false otherwise.
     * @throws IOException
     *             if an interval from commitIntervals contains a lower version version, then an overlapping interval
     *             from curVecIntervals.
     */
    static boolean calculateIntervalsToCommitAbort(List<Interval> commitIntervals, Interval reqInterval,
            List<Interval> curVecIntervals, List<Interval> nextVecIntervals, 
            List<Interval> toCommitAcc, List<Interval> toAbortAcc) throws IOException {
        assert (!commitIntervals.isEmpty());

        Interval emptyInterval = ObjectInterval.empty(commitIntervals.get(0).getOpStart(),
                commitIntervals.get(commitIntervals.size() - 1).getOpEnd());

        Iterator<Interval> curIt = curVecIntervals.iterator();
        Iterator<Interval> nextIt = nextVecIntervals.iterator();

        Interval curInterval = curIt.hasNext() ? curIt.next() : emptyInterval;
        Interval nextInterval = nextIt.hasNext() ? nextIt.next() : emptyInterval;
        
        // Check for every commit interval if it is available.
        for (Interval commitInterval : commitIntervals) {
            // Advance to the next interval that could be a possible match
            // or set an empty interval as a placeholder
            while (curInterval.getEnd() <= commitInterval.getStart() && curIt.hasNext()) {
                curInterval = curIt.next();
            }
            if (curInterval.getEnd() <= commitInterval.getStart()) {
                curInterval = emptyInterval;
            }

            // Advance to the next interval that could be a possible match
            // or set an empty interval as a placeholder
            while (nextInterval.getEnd() <= commitInterval.getStart() && nextIt.hasNext()) {
                nextInterval = nextIt.next();
            }
            if (nextInterval.getEnd() <= commitInterval.getStart()) {
                nextInterval = emptyInterval;
            }

            // Check if the interval exists in the current vector.
            // It could be, that the interval in the current vector is larger then the current one, because it has
            // not been split yet.
            // req:  |--1--|-2-|     or   |-2-|--1--|  or |-1-|-2-|-1-|
            // cur:  |----1----|          |----1----|     |-----1-----|
            // next: |     |-2-|          |-2-|     |     |   |-2-|   |
            // It is obvious, that intervals from next must have matching start/end positions also.

            if (commitInterval.equalsVersionId(curInterval)) {
                // If the version and the id match, they have to overlap
                assert (commitInterval.overlaps(curInterval));

                // Since the commitInterval is already in the curVector, overlapping intervals from next have to be
                // aborted.
                while (commitInterval.overlaps(nextInterval)) {
                    if (!nextInterval.isEmpty() && !nextInterval.equalsVersionId(reqInterval)) {
                        // ABORT/INVALIDATE
                        toAbortAcc.add(nextInterval);
                    }
                    
                    // Advance the nextInterval iterator or set an empty interval as a placeholder and stop the loop
                    if (nextIt.hasNext()) {
                        nextInterval = nextIt.next();
                    } else {
                        nextInterval = emptyInterval;
                        break;
                    }
                }

                
            } else if (!commitInterval.isEmpty()) {
                if (commitInterval.overlaps(curInterval)
                        && commitInterval.getVersion() < curInterval.getVersion()) {
                    // FAILED (should never happen)
                    // TODO (jdillmann): Log with better message.
                    throw new XtreemFSException("request interval is older then the current interval");
                }

                if (commitInterval.equals(nextInterval)) {
                    // COMMIT nextInterval
                    toCommitAcc.add(nextInterval);
                } else {
                    // FAILED (go into recovery)
                    return true;
                }
            }
        }
        
        return false;
    }

    
    void commitECData(String fileId, FileMetadata fi, Interval interval) throws IOException {
        StripingPolicyImpl sp = fi.getStripingPolicy();
        assert (interval.isOpComplete());
        long intervalStartOffset = interval.getStart();
        long intervalEndOffset = interval.getEnd() - 1; // end is exclusive

        long startObjNo = sp.getObjectNoForOffset(intervalStartOffset);
        long endObjNo = sp.getObjectNoForOffset(intervalEndOffset);

        boolean consistent = true;
        try {
            Iterator<Long> objNoIt = sp.getObjectsOfOSD(sp.getRelativeOSDPosition(), startObjNo, endObjNo);
            while (objNoIt.hasNext()) {
                Long objNo = objNoIt.next();

                long startOff = Math.max(sp.getObjectStartOffset(objNo), intervalStartOffset);
                long endOff = Math.min(sp.getObjectEndOffset(objNo), intervalEndOffset);

                String fileIdNext = fileId + ".next";
                int offset = (int) (startOff - sp.getObjectStartOffset(objNo));
                int length = (int) (endOff - startOff) + 1; // The byte from the end offset has to be included.
                int objVer = 1;
                // FIXME (jdillmann): Decide if/when sync should be used
                boolean sync = false;


                // TODO (jdillmann): Allow to copy data directly between files (bypass Buffer).
                ObjectInformation obj = layout.readObject(fileIdNext, fi, objNo, offset, length, objVer);

                ReusableBuffer buf = null; // will be freed by writeObject
                if (obj.getStatus() == ObjectStatus.EXISTS) {
                    buf = obj.getData();

                    int resultLength = buf.capacity();
                    if (resultLength < length) {
                        if (!buf.enlarge(length)) {
                            ReusableBuffer tmp = BufferPool.allocate(length);
                            tmp.put(buf);

                            BufferPool.free(buf);
                            buf = tmp;
                        }
                    }

                    buf.position(resultLength);
                } else {
                    buf = BufferPool.allocate(length);
                }

                while (buf.hasRemaining()) {
                    buf.put((byte) 0);
                }

                buf.flip();
                layout.writeObject(fileId, fi, buf, objNo, offset, objVer, sync, false);

                // Delete completely committed intervals.
                if (intervalStartOffset <= sp.getObjectStartOffset(objNo)
                        && intervalEndOffset >= sp.getObjectEndOffset(objNo)) {
                    layout.deleteObject(fileIdNext, fi, objNo, objVer);
                }

                consistent = false;

            }

            // Finally append the interval to the cur vector and "remove" it from the next vector
            fi.getECCurVector().insert(interval);
            layout.setECIntervalVector(fileId, Arrays.asList(interval), false, true);

            // Remove the interval by overwriting it with an empty interval
            Interval empty = ObjectInterval.empty(interval);
            layout.setECIntervalVector(fileId, Arrays.asList(empty), true, true);
            fi.getECNextVector().insert(empty);
            consistent = true;

        } catch (IOException ex) {
            if (!consistent) {
                // FIXME (jdillmann): Inform in detail about critical error
                Logging.logError(Logging.LEVEL_CRIT, this, ex);
            }
            throw ex;
        }

        // FIXME (jdillmann): truncate?
    }

    void abortECData(String fileId, FileMetadata fi, Interval interval) throws IOException {
        StripingPolicyImpl sp = fi.getStripingPolicy();
        long intervalStartOffset = interval.getStart();
        long intervalEndOffset = interval.getEnd() - 1; // end is exclusive
        long startObjNo = sp.getObjectNoForOffset(intervalStartOffset);
        long endObjNo = sp.getObjectNoForOffset(intervalEndOffset);
        int objVer = 1;

        boolean consistent = true;
        try {
            Iterator<Long> objNoIt = sp.getObjectsOfOSD(sp.getRelativeOSDPosition(), startObjNo, endObjNo);
            while (objNoIt.hasNext()) {
                Long objNo = objNoIt.next();

                // Delete the completely aborted objects
                if (intervalStartOffset <= sp.getObjectStartOffset(objNo) && intervalEndOffset >= sp.getObjectEndOffset(objNo)) {
                    layout.deleteObject(fileId, fi, objNo, objVer);
                    consistent = false;
                }
                // FIXME (jdillmann): Delete also, if the remaining partials are not set
                // => test on slice or iv first and iv last
                // TODO (jdillmann): Maybe overwrite partials with zeros.
            }

            // Remove the interval by overwriting it with an empty interval
            Interval empty = ObjectInterval.empty(interval);
            layout.setECIntervalVector(fileId, Arrays.asList(interval), true, true);
            fi.getECNextVector().insert(empty);
            // FIXME (jdillmann): Truncate the next vector if the interval start = 0 and end >= lastEnd
            // layout.setECIntervalVector(fileId, Collections.<Interval> emptyList(), true, false);
            consistent = true;

        } catch (IOException ex) {
            if (!consistent) {
                // FIXME (jdillmann): Inform in detail about critical error
                Logging.logError(Logging.LEVEL_CRIT, this, ex);
            }
            throw ex;
        }
    }
}
