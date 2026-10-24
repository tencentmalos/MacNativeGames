// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android;

import org.junit.Test;
import static org.junit.Assert.*;

public class PlatformTest {
    @Test public void testNativeVulkanRendering() {
        android.content.Context context=androidx.test.platform.app.InstrumentationRegistry.getInstrumentation().getTargetContext();
        java.io.File directory=new java.io.File(context.getCacheDir(),"nvn-self-test/shaders");
        assertTrue(directory.isDirectory() || directory.mkdirs());
        String report=Native.INSTANCE.vulkanProbe(directory.getAbsolutePath());
        android.util.Log.i("MacNativeTest",report);
        assertTrue(report,report.startsWith("PASS:"));
        // Repeat to prove session shutdown releases renderer-owned state.
        report=Native.INSTANCE.vulkanProbe(directory.getAbsolutePath());
        assertTrue(report,report.startsWith("PASS:"));
    }

    @Test public void testControllerSessionAndFocus() {
        Native n=Native.INSTANCE;long session=n.begin();
        try {
            assertTrue(n.device(session,42,1,"synthetic",new int[]{0},new int[]{1},new float[]{-1,1,0}));
            assertTrue(n.packet(session,42,1,1,0,new int[]{0,1},new int[]{0,1},new float[]{1,-1}));
            assertTrue(n.status().contains("0x2"));
            assertTrue(n.status().contains("32767"));
            assertTrue(n.packet(session,42,1,2,4,new int[]{},new int[]{},new float[]{}));
            assertTrue(n.status().contains("0x0"));
            n.end(session);
            assertFalse(n.packet(session,42,1,3,0,new int[]{0},new int[]{0},new float[]{1}));
        } finally { n.end(session); }
    }
    @Test public void testFoundationAudioCallbackConsumption() {
        String report=Native.INSTANCE.audioProbe();
        android.util.Log.i("MacNativeTest",report);
        assertTrue(report,report.contains("consumed_frames="));
        long frames=Long.parseLong(report.substring(report.indexOf("consumed_frames=")+16));
        assertTrue(report,frames>=2400);
    }
}
