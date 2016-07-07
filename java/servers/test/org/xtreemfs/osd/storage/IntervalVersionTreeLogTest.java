package org.xtreemfs.osd.storage;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.ObjectOutputStream;
import java.util.LinkedList;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.xtreemfs.foundation.IntervalVersionAVLTree;
import org.xtreemfs.foundation.IntervalVersionTree.Interval;

public class IntervalVersionTreeLogTest {

    static File file;

    @Before
    public void setUp() throws IOException {
        file = File.createTempFile("ivt", ".log");
    }

    @After
    public void tearDown() {
        file.delete();
    }

    @Test
    public void testLoad() throws IOException {
        fail("Not yet implemented");
    }

    @Test
    public void testLoadInputStream() throws IOException {
        IntervalVersionTreeLog logTree = new IntervalVersionTreeLog(file);
        
        IntervalVersionAVLTree tree;
        LinkedList<Interval> expected = new LinkedList<Interval>();

        ByteArrayInputStream in;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ObjectOutputStream obj_out = new ObjectOutputStream(out);
        
        // Single Interval
        obj_out.writeLong(0);
        obj_out.writeLong(128);
        obj_out.writeLong(1);
        obj_out.flush();
        in = new ByteArrayInputStream(out.toByteArray());
        tree = new IntervalVersionAVLTree();
        logTree.load(tree, in);
        expected.add(new Interval(0, 128, 1));
        assertEquals(expected, tree.getVersions(0, 512));

        // Two intervals with a gap
        obj_out.writeLong(384);
        obj_out.writeLong(512);
        obj_out.writeLong(2);
        obj_out.flush();
        in = new ByteArrayInputStream(out.toByteArray());
        tree = new IntervalVersionAVLTree();
        logTree.load(tree, in);
        expected.add(new Interval(384, 512, 2));
        assertEquals(expected, tree.getVersions(0, 512));

        // Add third interval in between
        obj_out.writeLong(128);
        obj_out.writeLong(384);
        obj_out.writeLong(3);
        obj_out.flush();
        in = new ByteArrayInputStream(out.toByteArray());
        tree = new IntervalVersionAVLTree();
        logTree.load(tree, in);
        expected.clear();
        expected.add(new Interval(0, 128, 1));
        expected.add(new Interval(128, 384, 3));
        expected.add(new Interval(384, 512, 2));
        assertEquals(expected, tree.getVersions(0, 512));
        
        
        // Overwrite the middle interval and part of the outer intervals
        obj_out.writeLong(64);
        obj_out.writeLong(448);
        obj_out.writeLong(4);
        obj_out.flush();
        in = new ByteArrayInputStream(out.toByteArray());
        tree = new IntervalVersionAVLTree();
        logTree.load(tree, in);
        expected.clear();
        expected.add(new Interval(0, 64, 1));
        expected.add(new Interval(64, 448, 4));
        expected.add(new Interval(448, 512, 2));
        assertEquals(expected, tree.getVersions(0, 512));

        // Test corrupt file:
        obj_out.writeLong(0);
        obj_out.flush();
        in = new ByteArrayInputStream(out.toByteArray());
        try { 
            tree = new IntervalVersionAVLTree();
            logTree.load(tree, in);
            fail();
        } catch (IOException e) {
            // expected if the input stream is not long triples.
            // test on error message?
        }
    }

    @Test
    public void testSaveOutputStream() throws IOException {
        IntervalVersionTreeLog logTree = new IntervalVersionTreeLog(file);

        IntervalVersionAVLTree tree = new IntervalVersionAVLTree();
        ByteArrayOutputStream out;

        ByteArrayOutputStream expected = new ByteArrayOutputStream();
        ObjectOutputStream expected_oos = new ObjectOutputStream(expected);

        tree.insert(new Interval(0, 512, 0));
        expected_oos.writeLong(0);
        expected_oos.writeLong(512);
        expected_oos.writeLong(0);
        expected_oos.flush();

        out = new ByteArrayOutputStream();
        logTree.save(tree, out);
        assertArrayEquals(expected.toByteArray(), out.toByteArray());


        tree.insert(new Interval(512, 1024, 1));
        expected.reset();
        expected_oos = new ObjectOutputStream(expected);
        expected_oos.writeLong(0);
        expected_oos.writeLong(512);
        expected_oos.writeLong(0);
        expected_oos.writeLong(512);
        expected_oos.writeLong(1024);
        expected_oos.writeLong(1);
        expected_oos.flush();

        out = new ByteArrayOutputStream();
        logTree.save(tree, out);
        assertArrayEquals(expected.toByteArray(), out.toByteArray());
    }

}
