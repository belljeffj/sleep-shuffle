package cloud.jefflab.sleepshuffle.ui

import android.app.Activity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import cloud.jefflab.sleepshuffle.Updater

/** Shown on Home only when there is something to act on; stays out of the way otherwise. */
@Composable
fun UpdateBanner(modifier: Modifier = Modifier) {
    val state by Updater.state.collectAsState()
    val activity = LocalContext.current as? Activity ?: return
    when (val s = state) {
        is Updater.State.Available -> BigButton(
            "Update to ${s.release.version}",
            modifier.fillMaxWidth(),
            sub = "Downloads now, then Android asks you to tap Install",
            minHeight = 60.dp,
            fontSize = 18.sp,
        ) { Updater.downloadAndInstall(activity, s.release) }
        is Updater.State.Downloading -> Text(
            "Downloading ${s.release.version}… ${s.percent}%",
            color = Pal.AmberDim,
            fontSize = 16.sp,
            modifier = modifier.padding(vertical = 8.dp),
        )
        else -> Unit
    }
}

/** Settings section: installed version, a manual check, and the result. */
@Composable
fun UpdateSection() {
    val state by Updater.state.collectAsState()
    val activity = LocalContext.current as? Activity ?: return
    val installed = Updater.installedVersion(activity)
    Column(Modifier.fillMaxWidth()) {
        Label("App version")
        Text("Installed: $installed", color = Pal.AmberDim, fontSize = 16.sp)
        Spacer(Modifier.height(8.dp))
        when (val s = state) {
            is Updater.State.Available ->
                BigButton("Update to ${s.release.version}", Modifier.fillMaxWidth(), minHeight = 60.dp) {
                    Updater.downloadAndInstall(activity, s.release)
                }
            is Updater.State.Downloading ->
                Text("Downloading… ${s.percent}%", color = Pal.AmberDim, fontSize = 16.sp)
            else -> {
                BigButton(
                    if (s is Updater.State.Checking) "Checking…" else "Check for updates",
                    Modifier.fillMaxWidth(),
                    minHeight = 60.dp,
                ) { if (s !is Updater.State.Checking) Updater.check(activity) }
                val note = when (s) {
                    is Updater.State.UpToDate -> "You have the latest version."
                    is Updater.State.Failed -> s.message
                    else -> null
                }
                note?.let { Text(it, color = Pal.Faint, fontSize = 15.sp, modifier = Modifier.padding(top = 8.dp)) }
            }
        }
    }
}
