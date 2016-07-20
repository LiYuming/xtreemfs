/*
 * Copyright (c) 2016 by Johannes Dillmann, Zuse Institute Berlin
 *
 * Licensed under the BSD License, see LICENSE file for details.
 *
 */
package org.xtreemfs.osd.ec;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.net.InetSocketAddress;
import java.util.Arrays;
import java.util.List;

import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TestRule;
import org.xtreemfs.common.xloc.StripingPolicyImpl;
import org.xtreemfs.foundation.buffer.BufferPool;
import org.xtreemfs.foundation.buffer.ReusableBuffer;
import org.xtreemfs.foundation.intervals.Interval;
import org.xtreemfs.foundation.logging.Logging;
import org.xtreemfs.foundation.logging.Logging.Category;
import org.xtreemfs.foundation.pbrpc.client.RPCAuthentication;
import org.xtreemfs.foundation.pbrpc.client.RPCNIOSocketClient;
import org.xtreemfs.foundation.pbrpc.client.RPCResponse;
import org.xtreemfs.foundation.pbrpc.generatedinterfaces.RPC.UserCredentials;
import org.xtreemfs.osd.OSDConfig;
import org.xtreemfs.osd.storage.FileMetadata;
import org.xtreemfs.osd.storage.HashStorageLayout;
import org.xtreemfs.osd.storage.MetadataCache;
import org.xtreemfs.osd.storage.ObjectInformation;
import org.xtreemfs.osd.storage.ObjectInformation.ObjectStatus;
import org.xtreemfs.pbrpc.generatedinterfaces.GlobalTypes.FileCredentials;
import org.xtreemfs.pbrpc.generatedinterfaces.GlobalTypes.OSDWriteResponse;
import org.xtreemfs.pbrpc.generatedinterfaces.OSD.ObjectData;
import org.xtreemfs.pbrpc.generatedinterfaces.OSD.xtreemfs_ec_commit_vectorRequest;
import org.xtreemfs.pbrpc.generatedinterfaces.OSD.xtreemfs_ec_commit_vectorResponse;
import org.xtreemfs.pbrpc.generatedinterfaces.OSDServiceClient;
import org.xtreemfs.test.SetupUtils;
import org.xtreemfs.test.TestEnvironment;
import org.xtreemfs.test.TestHelper;

public class ECReconstructionTest extends ECTestCommon {
    @Rule
    public final TestRule testLog = TestHelper.testLog;
    
    private TestEnvironment     testEnv;
    private OSDConfig[]         configs;
    private OSDServiceClient    osdClient;
    private List<String>        osdUUIDs;
    private FileCredentials     fc;
    private UserCredentials     userCredentials;
    private static final String fileId  = "ABCDEF:1";

    @BeforeClass
    public static void initializeTest() throws Exception {
        Logging.start(SetupUtils.DEBUG_LEVEL, SetupUtils.DEBUG_CATEGORIES);
        // Logging.start(Logging.LEVEL_INFO);
        // Logging.start(Logging.LEVEL_DEBUG);
        Category[] DEBUG_CATEGORIES = new Category[] {
                // Category.all,
                Category.buffer,
                // Category.lifecycle,
                // Category.net,
                Category.auth,
                // Category.stage,
                // Category.proc,
                Category.misc, 
                Category.storage, 
                Category.replication, 
                Category.tool, 
                Category.test, 
                // Category.flease,
                // Category.babudb,
                Category.ec };
        Logging.start(Logging.LEVEL_DEBUG, DEBUG_CATEGORIES);
    }

    @Before
    public void setUp() throws Exception {
        testEnv = new TestEnvironment(new TestEnvironment.Services[] { TestEnvironment.Services.DIR_SERVICE,
                TestEnvironment.Services.TIME_SYNC, TestEnvironment.Services.UUID_RESOLVER,
                TestEnvironment.Services.OSD_CLIENT, 
                TestEnvironment.Services.OSD, 
                TestEnvironment.Services.OSD,
                TestEnvironment.Services.OSD,
                TestEnvironment.Services.OSD,
                TestEnvironment.Services.OSD });
        testEnv.start();
        
        configs = testEnv.getOSDConfigs();

        osdClient = testEnv.getOSDClient();
        RPCNIOSocketClient client = new RPCNIOSocketClient(null, 5 * 60 * 1000, 10 * 60 * 1000, "EC1");
        osdClient = new OSDServiceClient(client, null);
        client.start();
        client.waitForStartup();

        osdUUIDs = Arrays.asList(testEnv.getOSDUUIDs());
        fc = getFileCredentials(fileId, 3, 2, 1, 4, osdUUIDs.subList(0, 5));
        userCredentials = UserCredentials.newBuilder().setUsername("test").addGroups("test").build();
    }

    @After
    public void tearDown() {
        testEnv.shutdown();
    }


    @Test
    public void testReconstructData() throws Exception {
        String fileId = "testReconstructData:1";
        FileCredentials fc = getFileCredentials(fileId, 3, 2, 1, 4, osdUUIDs.subList(0, 5));
        int dataWidth = 3;
        int chunkSize = 1024;

        int stopOsd = 1;

        InetSocketAddress masterAddress = electMaster(fileId, fc);

        long objNumber = 0;
        long objVersion = -1;
        int offset = 0;
        int length = 1;
        long lease_timeout = 0;

        RPCResponse<ObjectData> RPCReadResponse = null;
        ObjectData readResponse;

        RPCResponse<OSDWriteResponse> RPCWriteResponse;
        OSDWriteResponse writeResponse;
        ObjectData objData = ObjectData.newBuilder().setChecksum(0).setZeroPadding(0).setInvalidChecksumOnOsd(false)
                .build();
        ReusableBuffer data, dout, dataExpected;

        xtreemfs_ec_commit_vectorRequest commitRequest;
        xtreemfs_ec_commit_vectorRequest.Builder commitRequestBuilder;

        RPCResponse<xtreemfs_ec_commit_vectorResponse> rpcCommitResponse;
        xtreemfs_ec_commit_vectorResponse commitResponse;

        RPCResponse<?> triggerResponse;

        HashStorageLayout layout;
        StripingPolicyImpl sp;
        FileMetadata fi;
        ObjectInformation objInf;
        List<Interval> commitIntervals;

        // Write the whole first stripe over all three OSDs
        // ************************************************
        // Stop the OSD and do a write/read cycle to commit the vectors
        testEnv.stopOSD(osdUUIDs.get(stopOsd));

        length = dataWidth * chunkSize;
        data = SetupUtils.generateData(length);
        RPCWriteResponse = osdClient.write(masterAddress, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                fileId, objNumber, objVersion, offset, lease_timeout, objData, data.createViewBuffer());
        writeResponse = RPCWriteResponse.get();
        RPCWriteResponse.freeBuffers();

        RPCReadResponse = osdClient.read(masterAddress, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                fileId, objNumber, objVersion, offset, length);
        readResponse = RPCReadResponse.get();
        RPCReadResponse.freeBuffers();
        dout = RPCReadResponse.getData();
        RPCReadResponse = null;

        assertBufferEquals(data, dout);
        BufferPool.free(data);
        BufferPool.free(dout);

        layout = new HashStorageLayout(configs[0], new MetadataCache());
        sp = getStripingPolicyImplementation(fc);
        fi = layout.getFileMetadataNoCaching(sp, fileId);
        assertTrue(fi.getECNextVector().serialize().isEmpty());
        commitIntervals = fi.getECCurVector().serialize();

        // Restart the OSD and send a commit request
        testEnv.startOSD(osdUUIDs.get(stopOsd));
        Thread.sleep(1 * 1000);


        triggerResponse = osdClient.xtreemfs_ec_trigger_reconstruction(masterAddress, RPCAuthentication.authNone,
                RPCAuthentication.userService, fc, fileId, stopOsd);
        triggerResponse.get();
        triggerResponse.freeBuffers();

        Thread.sleep(10 * 1000);

        // Test reconstructed vector
        layout = new HashStorageLayout(configs[stopOsd], new MetadataCache());
        fi = layout.getFileMetadataNoCaching(sp, fileId);
        assertEquals(commitIntervals, fi.getECCurVector().serialize());

        // Test reconstructed data
        long reconstructedObjNo = objNumber + stopOsd;
        dataExpected = data.createViewBuffer();
        dataExpected.range(stopOsd * chunkSize, chunkSize);
        objInf = layout.readObject(fileId, fi, reconstructedObjNo, 0, chunkSize, 1);
        assertEquals(ObjectStatus.EXISTS, objInf.getStatus());
        assertBufferEquals(dataExpected, objInf.getData());
        BufferPool.free(objInf.getData());
        BufferPool.free(dataExpected);
        BufferPool.free(data);

        System.out.println("done");

    }


    @Test
    public void testReconstructDataVersion() throws Exception {
        String fileId = "testReconstructDataVersion:1";
        FileCredentials fc = getFileCredentials(fileId, 3, 2, 1, 4, osdUUIDs.subList(0, 5));
        int dataWidth = 3;
        int chunkSize = 1024;

        int stopOsd = 1;

        InetSocketAddress masterAddress = electMaster(fileId, fc);

        long objNumber = 0;
        long objVersion = -1;
        int offset = 0;
        int length = 1;
        long lease_timeout = 0;

        RPCResponse<ObjectData> RPCReadResponse = null;
        ObjectData readResponse;

        RPCResponse<OSDWriteResponse> RPCWriteResponse;
        OSDWriteResponse writeResponse;
        ObjectData objData = ObjectData.newBuilder().setChecksum(0).setZeroPadding(0).setInvalidChecksumOnOsd(false)
                .build();
        ReusableBuffer data, dout, dataExpected;

        xtreemfs_ec_commit_vectorRequest commitRequest;
        xtreemfs_ec_commit_vectorRequest.Builder commitRequestBuilder;

        RPCResponse<xtreemfs_ec_commit_vectorResponse> rpcCommitResponse;
        xtreemfs_ec_commit_vectorResponse commitResponse;

        RPCResponse<?> triggerResponse;

        HashStorageLayout layout;
        StripingPolicyImpl sp;
        FileMetadata fi;
        ObjectInformation objInf;
        List<Interval> commitIntervals;

        // Write the first and the third chunk, s.t. intervals are missing on the second OSD
        // *********************************************************************************
        // Stop the OSD and do a write/read cycle to commit the vectors
        testEnv.stopOSD(osdUUIDs.get(stopOsd));

        objNumber = 0;
        length = chunkSize;
        data = SetupUtils.generateData(length);
        RPCWriteResponse = osdClient.write(masterAddress, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                fileId, objNumber, objVersion, offset, lease_timeout, objData, data.createViewBuffer());
        writeResponse = RPCWriteResponse.get();
        RPCWriteResponse.freeBuffers();
        BufferPool.free(data);

        objNumber = 2;
        length = chunkSize;
        data = SetupUtils.generateData(length);
        RPCWriteResponse = osdClient.write(masterAddress, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                fileId, objNumber, objVersion, offset, lease_timeout, objData, data.createViewBuffer());
        writeResponse = RPCWriteResponse.get();
        RPCWriteResponse.freeBuffers();
        BufferPool.free(data);

        objNumber = 0;
        length = dataWidth * chunkSize;
        RPCReadResponse = osdClient.read(masterAddress, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                fileId, objNumber, objVersion, offset, length);
        readResponse = RPCReadResponse.get();
        RPCReadResponse.freeBuffers();
        dout = RPCReadResponse.getData();
        RPCReadResponse = null;
        BufferPool.free(dout);


        layout = new HashStorageLayout(configs[0], new MetadataCache());
        sp = getStripingPolicyImplementation(fc);
        fi = layout.getFileMetadataNoCaching(sp, fileId);
        assertTrue(fi.getECNextVector().serialize().isEmpty());
        commitIntervals = fi.getECCurVector().serialize();

        // Restart the OSD and send a commit request
        testEnv.startOSD(osdUUIDs.get(stopOsd));
        Thread.sleep(1 * 1000);


        triggerResponse = osdClient.xtreemfs_ec_trigger_reconstruction(masterAddress, RPCAuthentication.authNone,
                RPCAuthentication.userService, fc, fileId, stopOsd);
        triggerResponse.get();
        triggerResponse.freeBuffers();

        Thread.sleep(10 * 1000);

        // Test reconstructed vector
        layout = new HashStorageLayout(configs[stopOsd], new MetadataCache());
        fi = layout.getFileMetadataNoCaching(sp, fileId);
        assertEquals(commitIntervals, fi.getECCurVector().serialize());

        // Test reconstructed data
        long reconstructedObjNo = objNumber + stopOsd;
        dataExpected = data.createViewBuffer();
        dataExpected.range(stopOsd * chunkSize, chunkSize);
        objInf = layout.readObject(fileId, fi, reconstructedObjNo, 0, chunkSize, 1);
        assertEquals(ObjectStatus.EXISTS, objInf.getStatus());
        assertBufferEquals(dataExpected, objInf.getData());
        BufferPool.free(objInf.getData());
        BufferPool.free(dataExpected);
        BufferPool.free(data);

        System.out.println("done");

    }


    InetSocketAddress electMaster(String fileId, FileCredentials fc) throws Exception {
        return electMaster(fileId, fc, 0);
    }

    InetSocketAddress electMaster(String fileId, FileCredentials fc, int skip) throws Exception {
        long objNumber = 0;
        long objVersion = -1;
        int offset = 0;
        int length = 1;
        long lease_timeout = 0;

        RPCResponse<ObjectData> RPCReadResponse = null;
        ObjectData readResponse;

        skip = skip % configs.length;

        InetSocketAddress masterAddress = null;
        // Find (and elect) the master
        for (int i = skip; i < configs.length; i++) {
            try {
                InetSocketAddress address = configs[i].getUUID().getAddress();
                RPCReadResponse = osdClient.read(address, RPCAuthentication.authNone, RPCAuthentication.userService, fc,
                        fileId, objNumber, objVersion, offset, length);
                readResponse = RPCReadResponse.get();
                masterAddress = address;
                RPCReadResponse.freeBuffers();
                BufferPool.free(RPCReadResponse.getData());
                RPCReadResponse = null;
                break;

            } catch (Exception ex) {
                if (RPCReadResponse != null) {
                    RPCReadResponse.freeBuffers();
                    BufferPool.free(RPCReadResponse.getData());
                }
            }
        }

        if (masterAddress == null) {
            throw new Exception("Could not elect master");
        }
        return masterAddress;
    }
}
