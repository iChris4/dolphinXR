// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.NativeLibrary
import org.json.JSONArray

/**
 * The VR game patches of a game (Core/VRPatch), read from the native side as JSON. Enabling a
 * patch and changing its settings go back through NativeLibrary, so the C++ code stays the only
 * writer of the game's GameSettingsVR file.
 */
object QuestVrPatches {
    data class Setting(
        val name: String,
        val label: String,
        val description: String,
        val unit: String,
        val type: String,
        val min: Float,
        val max: Float,
        val step: Float,
        val defaultValue: Float,
        val value: Float,
        val options: List<String>
    )

    data class Patch(
        val id: String,
        val title: String,
        val author: String,
        val description: String,
        val origin: String,
        val enabled: Boolean,
        val state: String,
        val status: String,
        val error: String,
        val forThisGame: Boolean,
        val settings: List<Setting>
    )

    fun load(gameId: String, revision: Int): List<Patch> {
        val patches = ArrayList<Patch>()
        val array = JSONArray(NativeLibrary.GetVRPatches(gameId, revision))
        for (i in 0 until array.length()) {
            val patch = array.getJSONObject(i)
            val settings = ArrayList<Setting>()
            val settingsArray = patch.getJSONArray("settings")
            for (j in 0 until settingsArray.length()) {
                val setting = settingsArray.getJSONObject(j)
                val optionsArray = setting.getJSONArray("options")
                settings.add(
                    Setting(
                        setting.getString("name"),
                        setting.getString("label"),
                        setting.getString("description"),
                        setting.getString("unit"),
                        setting.getString("type"),
                        setting.getDouble("min").toFloat(),
                        setting.getDouble("max").toFloat(),
                        setting.getDouble("step").toFloat(),
                        setting.getDouble("default").toFloat(),
                        setting.getDouble("value").toFloat(),
                        List(optionsArray.length()) { optionsArray.getString(it) }
                    )
                )
            }
            patches.add(
                Patch(
                    patch.getString("id"),
                    patch.getString("title"),
                    patch.optString("author"),
                    patch.optString("description"),
                    patch.getString("origin"),
                    patch.getBoolean("enabled"),
                    patch.getString("state"),
                    patch.getString("status"),
                    patch.getString("error"),
                    patch.getBoolean("for_this_game"),
                    settings
                )
            )
        }
        return patches
    }
}

/** Turns a patch on or off for the game. */
class QuestVrPatchEnabledSetting(
    private val gameId: String,
    private val patchId: String,
    private var value: Boolean
) : AbstractBooleanSetting {
    override val isOverridden: Boolean = false

    // Patches start and stop while the game runs.
    override val isRuntimeEditable: Boolean = true

    override fun delete(settings: Settings): Boolean = false

    override val boolean: Boolean
        get() = value

    override fun setBoolean(settings: Settings, newValue: Boolean) {
        value = newValue
        NativeLibrary.SetVRPatchEnabled(gameId, patchId, newValue)
    }
}

/** A number or choice setting of a patch (a choice is its option's index). */
class QuestVrPatchFloatSetting(
    private val gameId: String,
    private val patchId: String,
    private val name: String,
    private var value: Float,
    private val defaultValue: Float
) : AbstractFloatSetting {
    override val isOverridden: Boolean
        get() = value != defaultValue

    override val isRuntimeEditable: Boolean = true

    override fun delete(settings: Settings): Boolean {
        setFloat(settings, defaultValue)
        return true
    }

    override val float: Float
        get() = value

    override fun setFloat(settings: Settings, newValue: Float) {
        value = newValue
        NativeLibrary.SetVRPatchSetting(gameId, patchId, name, newValue.toDouble())
    }
}

/** A switch setting of a patch. */
class QuestVrPatchBooleanSetting(
    private val gameId: String,
    private val patchId: String,
    private val name: String,
    private var value: Boolean,
    private val defaultValue: Boolean
) : AbstractBooleanSetting {
    override val isOverridden: Boolean
        get() = value != defaultValue

    override val isRuntimeEditable: Boolean = true

    override fun delete(settings: Settings): Boolean {
        setBoolean(settings, defaultValue)
        return true
    }

    override val boolean: Boolean
        get() = value

    override fun setBoolean(settings: Settings, newValue: Boolean) {
        value = newValue
        NativeLibrary.SetVRPatchSetting(gameId, patchId, name, if (newValue) 1.0 else 0.0)
    }
}
