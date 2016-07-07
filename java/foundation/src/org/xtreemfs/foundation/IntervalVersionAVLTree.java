/*
 * Copyright (c) 2015 by Jan Fajerski,
 *               Zuse Institute Berlin
 *
 * Licensed under the BSD License, see LICENSE file for details.
 *
 */
package org.xtreemfs.foundation;

import java.util.LinkedList;
import java.util.List;

/**
 * @author Jan Fajerski
 */
public class IntervalVersionAVLTree extends IntervalVersionTree {

    IntervalNode root;
    long         highest;

    public IntervalVersionAVLTree(long begin, long end) {
        if (begin != 0) {
            throw new IllegalArgumentException("IntervalVersionAVLTree must start at 0");
        }
        this.root = new IntervalNode(begin, end);
        this.highest = end;
    }

    public IntervalVersionAVLTree() {
        this.root = null;
        this.highest = 0;
    }

    @Override
    public void insert(long begin, long end, long version) {
        if (begin < 0) {
            throw new IllegalArgumentException("Intervals may only start at 0 or greater, not " + begin);
        }

        if (end <= begin) {
            throw new IllegalArgumentException("Intervals must be at least 1");
        }

        if (end >= highest) {
            this.highest = end;
            if (begin == 0) {
                this.root = new IntervalNode(begin, end, version);
            }
        }

        this.root = insert(begin, end, version, this.root);
    }

    /**
     * Insert interval from begin (inclusive) to end (exclusive) into the tree and balance.
     */
    IntervalNode insert(long begin, long end, long version, IntervalNode node) {
        if (node == null) {
            return new IntervalNode(begin, end, version);

        } else if (begin == node.interval.begin && end == node.interval.end) {
            // same interval...just set version
            node.interval.version = version;

        } else if (end <= node.interval.begin) {
            // new interval is left of current
            node.left = insert(begin, end, version, node.left);

        } else if (begin <= node.interval.begin && end < node.interval.end) {
            // new interval overlaps with a left portion of the current interval...relinquish overlapping part pass new interval to the left
            node.left = insert(begin, end, version, node.left);
            node.interval.begin = end;

        } else if (begin >= node.interval.end) {
            // new interval is left of current
            node.right = insert(begin, end, version, node.right);

        } else if (begin > node.interval.begin && end >= node.interval.end) {
            // new interval overlaps with a right portion of the current interval...relinquish overlapping part pass new interval to the right
            node.right = insert(begin, end, version, node.right);
            node.interval.end = begin;

        } else if (begin > node.interval.begin && end < node.interval.end) {
            // new interval fits into current interval
            IntervalNode newNode = new IntervalNode(begin, end, version);
            newNode.left = insert(node.interval.begin, begin, node.interval.version, node.left);
            newNode.right = insert(end, node.interval.end, node.interval.version, node.right);
            node = newNode;

        } else if (begin <= node.interval.begin && end >= node.interval.end) {
            // new interval surrounds current interval
            if (node.left != null && begin < node.interval.begin) {
                node.left = truncateMax(begin, node.left);
            }
            if (node.right != null && end > node.interval.end) {
                node.right = truncateMin(end, node.right);
            }
            node.interval.begin = begin;
            node.interval.end = end;
            node.interval.version = version;
        }

        return rotate(node);
    }

    static IntervalNode rotate(IntervalNode node) {
        node.checkHeight();

        if (node.balance < -1) {
            node = rotateLeft(node);
        }

        if (node.balance > 1) {
            node = rotateRight(node);
        }

        return node;
    }

    static IntervalNode rotateRight(IntervalNode node) {
        IntervalNode h = node.left;
        node.left = node.left.right;
        h.right = node;

        node.checkHeight();
        h.checkHeight();

        return h;
    }

    static IntervalNode rotateLeft(IntervalNode node) {
        IntervalNode h = node.right;
        node.right = node.right.left;
        h.left = node;

        node.checkHeight();
        h.checkHeight();

        return h;
    }

    IntervalNode truncateMax(long max, IntervalNode node) {
        if (node == null) {
            // The whole tree got truncated.
            return node;
        }

        if (max <= node.interval.begin) {
            // The max is left of the current interval.
            // Drop the node and its right subtree and truncate the remaining left subtree.
            return truncateMax(max, node.left);

        } else if (max > node.interval.end) {
            // The max is right of the current interval.
            // Truncate the right subtree and return the rotated current interval.
            node.right = truncateMax(max, node.right);
            return rotate(node);

        } else { // if (max > node.interval.begin && max <= node.interval.end) {
            // The max is within the current interval.
            // Adjust it and drop its right subtree and return it.
            node.interval.end = max;
            node.right = null;
            return rotate(node);
        }
    }

    IntervalNode truncateMin(long min, IntervalNode node) {
        if (node == null) {
            // The whole tree got truncated.
            return node;
        }

        if (min >= node.interval.end) {
            // The min is right of the current interval.
            // Drop the node and its left subtree and truncate the remaining right subtree.
            return truncateMin(min, node.right);

        } else if (min < node.interval.begin) {
            // The min is left of the current interval.
            // Truncate the left subtree and return the rotated current interval.
            node.left = truncateMin(min, node.left);
            return rotate(node);

        } else { // if (min >= node.interval.begin && min < node.interval.end) {
            // The min is within the current interval.
            // Adjust it and drop its left subtree and return it.
            node.interval.begin = min;
            node.left = null;
            return rotate(node);
        }
    }

    @Override
    public LinkedList<Interval> getVersions(long begin, long end) {
        LinkedList<Interval> versions = new LinkedList<Interval>();
        getVersions(begin, end, this.root, versions);
        return versions;
    }

    static void getVersions(long begin, long end, IntervalNode node, LinkedList<Interval> acc) {

        if (node == null) {
            return;

        } else if (begin >= node.interval.begin && end <= node.interval.end) {
            // The lookup interval is completely covered by the current interval
            addInterval(acc, begin, end, node.interval.version);

        } else if (begin < node.interval.begin && end <= node.interval.end) {
            // The lookup interval is beginning left of the current interval: descend left
            getVersions(begin, end, node.left, acc);

            // If the lookup and the current interval overlap add the overlap to the result
            if (end > node.interval.begin) {
                addInterval(acc, node.interval.begin, end, node.interval.version);
            }

        } else if (begin >= node.interval.begin && end > node.interval.end) {
            // If the lookup and the current interval overlap add the overlap to the result
            if (begin < node.interval.end) {
                addInterval(acc, begin, node.interval.end, node.interval.version);
            }

            // The lookup interval is ending right of the current interval: descend right
            getVersions(begin, end, node.right, acc);

        } else if (begin < node.interval.begin && end > node.interval.end) {
            getVersions(begin, end, node.left, acc);
            addInterval(acc, node.interval);
            getVersions(begin, end, node.right, acc);
        }
    }

    /**
     * Add the interval defined by begin, end and version to list. If the intervals lines up with the last interval in
     * the list, and their version matches, the last interval is simply expanded.
     * 
     * @param acc
     *            list of intervals
     * @param begin
     * @param end
     * @param version
     */
    static void addInterval(LinkedList<Interval> acc, long begin, long end, long version) {
        Interval last = acc.peekLast();
        if (last != null && begin <= last.end && last.version == version) {
            // There is no gap between the last and current interval and the version matches:
            // The last interval can simply be expanded.
            last.end = end;
        } else {
            acc.add(new Interval(begin, end, version));
        }

    }

    /**
     * @see #addInterval(LinkedList, long, long, long)
     * @param acc
     *            list of intervals
     * @param i
     *            interval to add
     */
    static void addInterval(LinkedList<Interval> acc, Interval i) {
        addInterval(acc, i.begin, i.end, i.version);
    }

    /**
     * Merge successive intervals with the same version to create a compact tree.
     * 
     * @return compacted IntervalVersionAVLTree
     */
    IntervalVersionAVLTree compact() {
        LinkedList<Interval> intervals = new LinkedList<Interval>();
        compact(root, intervals);
        
        IntervalVersionAVLTree tree = new IntervalVersionAVLTree();
        for (Interval i : intervals) {
            // TODO (jdillmann): Could be optimized
            tree.insert(i);
        }
        
        return tree;
    }

    static void compact(IntervalNode node, LinkedList<Interval> acc) {
        if (node == null) {
            return;
        }

        compact(node.left, acc);
        addInterval(acc, node.interval);
        compact(node.right, acc);
    }

    public List<Interval> serialize() {
        LinkedList<Interval> intervals = new LinkedList<Interval>();
        compact(root, intervals);
        return intervals;
    }

    /**
     * Truncate the IntervalTree to end at max.
     * 
     * @param max
     */
    // TODO (jdillmann): Rename to truncateMax
    public void truncate(long max) {
        if (max < 0) {
            throw new IllegalArgumentException("Cannot truncate to negative values.");
        }

        if (max < this.highest) {
            this.highest = max;
            root = truncateMax(max, root);
        }
    }

    /**
     * Worst case guess of the number of intervals in this tree (including the node itself)
     */
    int maxIntervals(IntervalNode node) {
        if (node == null)
            return 0;

        return 1 + (2 ^ node.height - 1) + (2 ^ (node.height - Math.abs(node.balance)) - 1);
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder();
        toString(root, sb);
        if (sb.length() > 0) {
            sb.setLength(sb.length() - 1);
        }
        return sb.toString();
    }

    static void toString(IntervalNode node, StringBuilder sb) {
        if (node == null) {
            return;
        }

        toString(node.left, sb);
        
        sb.append(node.interval.toString());
        sb.append(" ");

        toString(node.right, sb);
    }

    /*
     * A node contains the left and the right branch, the middle index of this node and a lists of intervals.
     * The list contains all intervals intersecting with the index.
     */
    static class IntervalNode {
        Interval interval;
        IntervalNode left;
        IntervalNode right;
        int balance;
        int height;

        IntervalNode(long begin, long end) {
            this.interval = new Interval(begin, end);
            this.left = null;
            this.right = null;
            height = 0;
            this.balance = 0;
        }

        IntervalNode(long begin, long end, long version) {
            this.interval = new Interval(begin, end, version);
            this.left = null;
            this.right = null;
            height = 0;
            this.balance = 0;
        }

        void checkHeight() {
            int l_h = this.left == null ? 0 : this.left.height + 1;
            int r_h = this.right == null ? 0 : this.right.height + 1;
            this.height = Math.max(l_h, r_h);
            this.balance = l_h - r_h;
        }

        @Override
        public String toString() {
            return interval.toString();
        }
    }
}
