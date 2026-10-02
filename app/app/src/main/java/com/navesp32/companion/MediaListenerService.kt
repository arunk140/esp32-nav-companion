package com.navesp32.companion

import android.content.ComponentName
import android.content.Context
import android.service.notification.NotificationListenerService
import androidx.core.app.NotificationManagerCompat

// Exists only so the user can grant Notification access: Android requires an
// enabled NotificationListenerService before an app may read other apps'
// media sessions (MediaSessionManager.getActiveSessions). It does nothing.
class MediaListenerService : NotificationListenerService() {
    companion object {
        fun component(ctx: Context) = ComponentName(ctx, MediaListenerService::class.java)

        fun hasAccess(ctx: Context): Boolean =
            NotificationManagerCompat.getEnabledListenerPackages(ctx).contains(ctx.packageName)
    }
}
