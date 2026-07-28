// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import androidx.fragment.app.FragmentActivity
import org.dolphinemu.dolphinemu.dialogs.QuestControllerSetupDialog
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.features.settings.model.Settings

object FirstLaunchDialogs {
    @JvmStatic
    fun checkInit(activity: FragmentActivity) {
        AfterDirectoryInitializationRunner().runWithLifecycle(activity) {
            if (QuestVrSettings.shouldAskAboutControllerSetup()) {
                val fragmentManager = activity.supportFragmentManager
                if (fragmentManager.findFragmentByTag(QuestControllerSetupDialog.TAG) == null) {
                    QuestControllerSetupDialog.newInstance(true).show(
                        fragmentManager,
                        QuestControllerSetupDialog.TAG
                    )
                }
            } else {
                Analytics.checkAnalyticsInit(activity)
            }
        }
    }

    fun onQuestControllerSetupAnswered(activity: FragmentActivity, applyDefaults: Boolean) {
        Settings().use { settings ->
            settings.loadSettings()
            QuestVrSettings.recordControllerSetupChoice(settings, applyDefaults)
            settings.saveSettings()
        }

        // The Quest dialog is dismissed after its button callback returns. Queue analytics for the
        // next UI pass so the two dialogs never overlap one another.
        activity.window.decorView.post { Analytics.checkAnalyticsInit(activity) }
    }
}
