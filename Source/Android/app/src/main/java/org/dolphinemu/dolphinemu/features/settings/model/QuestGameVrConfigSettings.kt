// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import java.io.File
import java.util.Locale

object QuestGameVrConfigSettings {
    private const val VR_SECTION = "Graphics.VR"
    private const val LEGACY_VR_SECTION = "GFX.VR"

    // Every key the VR Config screen can read back. A key missing here is dropped while parsing,
    // so a stored per-game value would silently read as the global default.
    private val knownKeys = setOf(
        "EnableOpenXR",
        "UnitsPerMeter",
        "EnableLeanBackAngle",
        "LeanBackAngle",
        "EnableCameraForward",
        "CameraForward",
        "EnableCameraHeight",
        "CameraHeight",
        "EnableCameraAnchor",
        "CameraAnchorSmoothing",
        "EnableControllerAnchor",
        "VirtualScreen",
        "ScreenDistance",
        "ScreenSize",
        "HudThickness",
        "HeadLockedCurvature",
        "ExactScreenDepth",
        "AutoNativeEfbEffects",
        "DontClearScreen",
        "LoadCustomShaders",
        "DisableCPUCull",
        "RemoveCinematicBars",
        "FrameSizeFromXFB",
        "SmallViewportsOnScreen",
        "DetectRenderTargets",
        "OrthoScissorFix",
        "DetectSkybox",
        "MetroidThermalVisorFix",
        "MetroidD3DThermalPaletteFix",
        "ForcedVBIFrequency",
        "EagerHeartbeat",
        "ClearEFBCopies",
        "Gamma",
        "ResolutionScale",
        "FoveationLevel",
        "DynamicFoveation",
        "FoveateEFB",
        "UseVulkanMultiview",
        "AndroidDirectToHMD",
        "Passthrough",
        "PassthroughRemoveBlackBackground",
        "PassthroughRemoveBlackEFBClears",
        "PassthroughSceneOpacity",
        "PassthroughCoverageMode"
    )

    fun getStoredKeys(gameId: String, revision: Int): Set<String> =
        readMergedValues(gameId, revision).keys

    fun hasStoredValues(gameId: String, revision: Int): Boolean =
        getStoredKeys(gameId, revision).isNotEmpty()

    fun getBoolean(gameId: String, revision: Int, key: String, defaultValue: Boolean): Boolean =
        parseBoolean(readMergedValues(gameId, revision)[key]) ?: defaultValue

    fun setBoolean(gameId: String, key: String, value: Boolean) {
        writeLocalValue(gameId, key, if (value) "True" else "False")
    }

    fun getInt(gameId: String, revision: Int, key: String, defaultValue: Int): Int =
        readMergedValues(gameId, revision)[key]?.toIntOrNull() ?: defaultValue

    fun setInt(gameId: String, key: String, value: Int) {
        writeLocalValue(gameId, key, value.toString())
    }

    fun getFloat(gameId: String, revision: Int, key: String, defaultValue: Float): Float =
        readMergedValues(gameId, revision)[key]?.toFloatOrNull() ?: defaultValue

    fun setFloat(gameId: String, key: String, value: Float) {
        writeLocalValue(gameId, key, value.toString())
    }

    fun hasLocalValue(gameId: String, key: String): Boolean =
        readVRSectionValues(getUserFile(gameId)).containsKey(key)

    fun deleteLocalValue(gameId: String, key: String): Boolean {
        val file = getUserFile(gameId)
        if (!file.isFile) {
            return false
        }

        val lines = file.readLines()
        val updated = ArrayList<String>()
        var inVrSection = false
        var changed = false

        for (line in lines) {
            val trimmed = line.removeSuffix("\r").trim()
            if (isSectionHeader(trimmed)) {
                inVrSection = isVRSectionHeader(trimmed)
                updated.add(line.removeSuffix("\r"))
                continue
            }

            if (inVrSection && isKeyLine(trimmed, key)) {
                changed = true
                continue
            }

            updated.add(line.removeSuffix("\r"))
        }

        if (changed) {
            file.writeText(updated.joinToString("\n").trimEnd() + "\n")
        }
        return changed
    }

    private fun readMergedValues(gameId: String, revision: Int): Map<String, String> {
        val values = LinkedHashMap<String, String>()
        val filenames = getGameIniFilenames(gameId, revision)

        filenames.forEach { filename ->
            values.putAll(readVRSectionValues(getSysIniFile(filename)))
        }
        filenames.forEach { filename ->
            values.putAll(readVRSectionValues(getUserIniFile(filename)))
        }

        return values
    }

    private fun readVRSectionValues(file: File): Map<String, String> {
        if (!file.isFile) {
            return emptyMap()
        }

        val values = LinkedHashMap<String, String>()
        var inVrSection = false

        file.readLines().forEach { rawLine ->
            val line = rawLine.removeSuffix("\r")
            val trimmed = line.trim()
            if (trimmed.isEmpty()) {
                return@forEach
            }

            if (isSectionHeader(trimmed)) {
                inVrSection = isVRSectionHeader(trimmed)
                return@forEach
            }

            if (!inVrSection || trimmed.startsWith("#") || trimmed.startsWith(";") ||
                trimmed.startsWith("$") || trimmed.startsWith("*")
            ) {
                return@forEach
            }

            val separator = trimmed.indexOf('=')
            if (separator < 0) {
                return@forEach
            }

            val key = canonicalKey(trimmed.substring(0, separator).trim()) ?: return@forEach
            values[key] = trimmed.substring(separator + 1).trim()
        }

        return values
    }

    private fun writeLocalValue(gameId: String, key: String, value: String) {
        val file = getUserFile(gameId)
        val lines = if (file.isFile) file.readLines() else emptyList()
        val updated = ArrayList<String>()
        var inVrSection = false
        var foundVrSection = false
        var wroteValue = false

        for (line in lines) {
            val cleanLine = line.removeSuffix("\r")
            val trimmed = cleanLine.trim()

            if (isSectionHeader(trimmed)) {
                inVrSection = isVRSectionHeader(trimmed)
                updated.add(cleanLine)

                if (inVrSection) {
                    foundVrSection = true
                    updated.add("$key = $value")
                    wroteValue = true
                }
                continue
            }

            if (inVrSection && isKeyLine(trimmed, key)) {
                continue
            }

            updated.add(cleanLine)
        }

        if (!foundVrSection) {
            if (updated.isNotEmpty() && updated.last().isNotEmpty()) {
                updated.add("")
            }
            updated.add("[$VR_SECTION]")
            updated.add("$key = $value")
        } else if (!wroteValue) {
            updated.add("$key = $value")
        }

        file.parentFile?.mkdirs()
        file.writeText(updated.joinToString("\n").trimEnd() + "\n")
    }

    private fun isKeyLine(line: String, key: String): Boolean {
        val separator = line.indexOf('=')
        if (separator < 0) {
            return false
        }
        return line.substring(0, separator).trim().equals(key, ignoreCase = true)
    }

    private fun canonicalKey(key: String): String? =
        knownKeys.firstOrNull { it.equals(key, ignoreCase = true) }

    private fun isSectionHeader(line: String): Boolean =
        line.length >= 3 && line.startsWith("[") && line.endsWith("]")

    private fun isVRSectionHeader(line: String): Boolean {
        if (!isSectionHeader(line)) {
            return false
        }

        val section = line.substring(1, line.length - 1)
        return section.equals(VR_SECTION, ignoreCase = true) ||
            section.equals(LEGACY_VR_SECTION, ignoreCase = true)
    }

    private fun parseBoolean(value: String?): Boolean? {
        return when (value?.trim()?.lowercase(Locale.ROOT)) {
            "true", "1", "yes", "on" -> true
            "false", "0", "no", "off" -> false
            else -> null
        }
    }

    private fun getUserFile(gameId: String): File =
        File(DirectoryInitialization.getUserDirectory(), "GameSettingsVR/$gameId.ini")

    private fun getSysIniFile(filename: String): File =
        File(DirectoryInitialization.getSysDirectory(), "GameSettingsVR/$filename")

    private fun getUserIniFile(filename: String): File =
        File(DirectoryInitialization.getUserDirectory(), "GameSettingsVR/$filename")

    private fun getGameIniFilenames(gameId: String, revision: Int): List<String> {
        if (gameId.isEmpty()) {
            return emptyList()
        }

        val filenames = ArrayList<String>()
        if (gameId.length == 6) {
            filenames.add("${gameId.substring(0, 1)}.ini")
            filenames.add("${gameId.substring(0, 3)}.ini")
        }
        filenames.add("$gameId.ini")
        if (revision != 0) {
            filenames.add("${gameId}r$revision.ini")
        }
        return filenames
    }
}

class QuestGameVrConfigBooleanSetting(
    private val gameId: String,
    private val revision: Int,
    private val key: String,
    private val defaultValue: Boolean
) : AbstractBooleanSetting {
    override val isOverridden: Boolean
        get() = QuestGameVrConfigSettings.hasLocalValue(gameId, key)

    override val isRuntimeEditable: Boolean
        get() = NativeLibrary.IsUninitialized()

    override fun delete(settings: Settings): Boolean =
        QuestGameVrConfigSettings.deleteLocalValue(gameId, key)

    override val boolean: Boolean
        get() = QuestGameVrConfigSettings.getBoolean(gameId, revision, key, defaultValue)

    override fun setBoolean(settings: Settings, newValue: Boolean) {
        QuestGameVrConfigSettings.setBoolean(gameId, key, newValue)
    }
}

class QuestGameVrConfigIntSetting(
    private val gameId: String,
    private val revision: Int,
    private val key: String,
    private val defaultValue: Int
) : AbstractIntSetting {
    override val isOverridden: Boolean
        get() = QuestGameVrConfigSettings.hasLocalValue(gameId, key)

    override val isRuntimeEditable: Boolean
        get() = NativeLibrary.IsUninitialized()

    override fun delete(settings: Settings): Boolean =
        QuestGameVrConfigSettings.deleteLocalValue(gameId, key)

    override val int: Int
        get() = QuestGameVrConfigSettings.getInt(gameId, revision, key, defaultValue)

    override fun setInt(settings: Settings, newValue: Int) {
        QuestGameVrConfigSettings.setInt(gameId, key, newValue)
    }
}

class QuestGameVrConfigFloatSetting(
    private val gameId: String,
    private val revision: Int,
    private val key: String,
    private val defaultValue: Float
) : AbstractFloatSetting {
    override val isOverridden: Boolean
        get() = QuestGameVrConfigSettings.hasLocalValue(gameId, key)

    override val isRuntimeEditable: Boolean
        get() = NativeLibrary.IsUninitialized()

    override fun delete(settings: Settings): Boolean =
        QuestGameVrConfigSettings.deleteLocalValue(gameId, key)

    override val float: Float
        get() = QuestGameVrConfigSettings.getFloat(gameId, revision, key, defaultValue)

    override fun setFloat(settings: Settings, newValue: Float) {
        QuestGameVrConfigSettings.setFloat(gameId, key, newValue)
    }
}
