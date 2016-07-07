/*
 * Copyright (c) 2016 by Johannes Dillmann
 *               Zuse Institute Berlin
 *
 * Licensed under the BSD License, see LICENSE file for details.
 *
 */
package org.xtreemfs.foundation.intervals;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;

// FIXME (jdillmann): Rename
public class ImmutableListIntervalVector extends IntervalVector {

    final List<Interval> intervals;
    // TODO (jdillmann): Store/Cache maxversion

    public ImmutableListIntervalVector(IntervalVector vector) {
        this.intervals = vector.serialize();
    }

    /**
     * Initialize an empty ListIntervalVector, to which sorted, gapless and non overlapping intervals can be appended
     * with {@link #append(Interval)}.
     */
    public ImmutableListIntervalVector() {
        this.intervals = new ArrayList<Interval>();
    }

    /**
     * Initialize a ListIntervalVector from the interval list. <br>
     * Note: The interval list has to be sorted and may not contain any gaps or overlaps.
     * 
     * @param intervals
     */
    public ImmutableListIntervalVector(List<Interval> intervals) {
        this.intervals = intervals;
    }

    @Override
    public IntervalVector getOverlapping(long start, long end) {
        LinkedList<Interval> result = new LinkedList<Interval>();

        for (Interval i : intervals) {
            if (i.end > start && i.start <= end) {
                addInterval(result, i);
            }
        }

        return new ImmutableListIntervalVector(result);
    }

    @Override
    public IntervalVector getSlice(long start, long end) {
        LinkedList<Interval> result = new LinkedList<Interval>();

        for (Interval i : intervals) {
            if (i.end > start && i.start <= end) {
                addInterval(result, i);
            }
        }

        sliceIntervalList(result, start, end);
        return new ImmutableListIntervalVector(result);
    };

    @Override
    public void insert(Interval i) {
        throw new UnsupportedOperationException();
    }

    /**
     * Append the interval to this vector if it does not overlap with any segment.<br>
     * Note: Gaps will be filled.
     * 
     * @param interval
     *            to append.
     * @throws IllegalArgumentException
     *             if the interval overlaps
     */
    public void append(Interval interval) {
        int n = intervals.size();
        if (n == 0) {
            intervals.add(interval);
            return;
        }

        Interval last = intervals.get(n - 1);
        if (last.end > interval.start) {
            throw new IllegalAccessError("Interval to append has to start after the last interval in this vector.");
        }

        if (last.end < interval.start) {
            // add gap interval
            Interval gap = new Interval(last.end, interval.start);
            intervals.add(gap);
        }

        intervals.add(interval);
    }

    @Override
    public long getMaxVersion() {
        long max = -1;
        for (Interval i : intervals) {
            if (i.version > max) {
                max = i.version;
            }
        }
        return max;
    }

    @Override
    public boolean isMaxVersionGreaterThen(IntervalVector o) {
        long otherMax = o.getMaxVersion();

        boolean isGreater = false;
        for (Interval i : intervals) {
            if (i.version > otherMax) {
                // Tests if the maxversion of any range of a version vector
                // is greater then that of the passed version vector
                isGreater = true;
            } else if (i.version < otherMax) {
                // The maxversion of all ranges has to be at least greater or equal
                return false;
            }
        }

        return isGreater;
    }

    @Override
    public boolean compareLEQThen(IntervalVector otherVector) {
        List<Interval> other = otherVector.serialize();

        Iterator<Interval> thisIt = intervals.iterator();
        Iterator<Interval> otherIt = other.iterator();
        Interval thisIv = null;
        Interval otherIv = null;
        
        if (thisIt.hasNext()) {
            thisIv = thisIt.next();
        }

        if (otherIt.hasNext()) {
            otherIv = otherIt.next();
        }

        // If both vectors are empty, they are by definition the same.
        if (otherIv == null && thisIv == null) {
            return true;
        }

        // If the vectors do not have the same start, the comparison is undefined.
        if (!(thisIv != null && otherIv != null) || (thisIv.start != otherIv.start)) {
            throw new IllegalArgumentException("IntervalVectors to compare have to be aligend.");
        }

        while (thisIv != null && otherIv != null) {


            if (thisIv.version > otherIv.version) {
                // This vector element version is greater than the other.
                return false;

            } else if (thisIv.version == otherIv.version
                    && !(thisIv.start == otherIv.start && thisIv.end == otherIv.end && thisIv.id == otherIv.id)) {
                // The vector element versions are the same, but the elements are not equal.
                return false;

            }

            if (thisIv.end > otherIv.end) {
                // Advance other vector
                otherIv = null;
            } else if (thisIv.end < otherIv.end) {
                // Advance this vector
                thisIv = null;
            } else {
                // Advance both vectors
                thisIv = null;
                otherIv = null;
            }

            if (thisIv == null && thisIt.hasNext()) {
                thisIv = thisIt.next();
            }

            if (otherIv == null && otherIt.hasNext()) {
                otherIv = otherIt.next();
            }
        }

        if (!(thisIv == null && otherIv == null)) {
            throw new IllegalArgumentException("IntervalVectors to compare have to be aligend.");
        }

        return true;
    }

    @Override
    public List<Interval> serialize() {
        return Collections.unmodifiableList(intervals);
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder();
        for (Interval interval : intervals) {
            sb.append(interval.toString()).append(" ");
        }

        if (sb.length() > 0) {
            sb.setLength(sb.length() - 1);
        }
        return sb.toString();
    }
}
