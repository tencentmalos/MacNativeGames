// SPDX-License-Identifier: MIT
package org.macnative.android

import android.app.Service
import android.content.Intent
import android.os.IBinder
import java.io.FileDescriptor
import java.io.PrintWriter

object PortProfiler {
    init { System.loadLibrary("macnative_platform") }
    external fun command(request: String): String
}

/** Debug manifest only. Commands never acquire a guest or renderer mutex. */
class PortDebugService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY
    override fun dump(fd: FileDescriptor, writer: PrintWriter, args: Array<out String>?) {
        writer.print(PortProfiler.command(args?.joinToString(" ") ?: "profiler_ring status"))
    }
}
