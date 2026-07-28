// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.dialogs

import android.app.Dialog
import android.os.Bundle
import android.widget.Toast
import androidx.fragment.app.DialogFragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.utils.FirstLaunchDialogs

class QuestControllerSetupDialog : DialogFragment() {
    private val isFirstLaunch: Boolean
        get() = arguments?.getBoolean(KEY_FIRST_LAUNCH) ?: true

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The first-launch prompt gates the analytics prompt behind its answer, so it has to be
        // answered. Invoked from the main menu it is just an action the user can back out of.
        isCancelable = !isFirstLaunch
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.quest_controller_setup_title)
            .setMessage(R.string.quest_controller_setup_description)
            .setPositiveButton(R.string.yes) { _, _ -> onAnswered(true) }
            .setNegativeButton(R.string.no) { _, _ -> onAnswered(false) }
            .create()
    }

    private fun onAnswered(applyDefaults: Boolean) {
        if (isFirstLaunch) {
            FirstLaunchDialogs.onQuestControllerSetupAnswered(requireActivity(), applyDefaults)
            return
        }

        // Applying on demand deliberately leaves the first-launch bookkeeping alone, so declining
        // here does not suppress the initial prompt.
        if (!applyDefaults) {
            return
        }

        Settings().use { settings ->
            settings.loadSettings()
            QuestVrSettings.applyDefaultControllerSetup(settings)
            settings.saveSettings()
        }

        Toast.makeText(
            requireContext(),
            R.string.quest_controller_preset_applied,
            Toast.LENGTH_SHORT
        ).show()
    }

    companion object {
        const val TAG = "QuestControllerSetupDialog"
        private const val KEY_FIRST_LAUNCH = "first_launch"

        fun newInstance(isFirstLaunch: Boolean): QuestControllerSetupDialog =
            QuestControllerSetupDialog().apply {
                arguments = Bundle().apply { putBoolean(KEY_FIRST_LAUNCH, isFirstLaunch) }
            }
    }
}
