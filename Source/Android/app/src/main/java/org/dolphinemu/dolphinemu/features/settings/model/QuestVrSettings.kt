// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.EmulatedController

object QuestVrSettings {
    const val STEREO_MODE_OPENXR = 6

    const val GC_PROFILE_NAME = "Quest Touch GameCube.ini"
    const val WIIMOTE_PROFILE_NAME = "OpenXR Wii Remote.ini"
    const val HOTKEY_PROFILE_NAME = "Quest.ini"
    private const val OPENXR_DEVICE = "OpenXR/0/OpenXR Controller"
    private const val VR_SECTION = "VR"

    private fun androidBooleanSetting(key: String, defaultValue: Boolean) =
        AdHocBooleanSetting(Settings.FILE_DOLPHIN, Settings.SECTION_INI_ANDROID, key, defaultValue)

    private fun androidIntSetting(key: String, defaultValue: Int) =
        AdHocIntSetting(Settings.FILE_DOLPHIN, Settings.SECTION_INI_ANDROID, key, defaultValue)

    private fun vrBooleanSetting(key: String, defaultValue: Boolean) =
        AdHocBooleanSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    private fun vrIntSetting(key: String, defaultValue: Int) =
        AdHocIntSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    private fun vrFloatSetting(key: String, defaultValue: Float) =
        AdHocFloatSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    fun isQuestBuild(): Boolean = BuildConfig.IS_QUEST

    fun openXrEnabledSetting() = androidBooleanSetting("QuestOpenXREnabled", true)

    fun launchInVrSetting() = androidBooleanSetting("QuestLaunchInVr", true)

    fun flatScreenSetting() = vrBooleanSetting("FlatScreen", false)

    fun recenterOnLaunchSetting() = androidBooleanSetting("QuestRecenterOnLaunch", true)

    fun leftHandedSetting() = androidBooleanSetting("QuestLeftHanded", false)

    fun showMirrorSurfaceSetting() = androidBooleanSetting("QuestShowMirrorSurface", false)

    // Superseded by ForcedVBIFrequency; kept only so an explicit choice can clear a stale value.
    private fun autoVbiFromHmdSetting() = vrBooleanSetting("AutoVBIFromHMD", false)

    fun forcedVbiFrequencySetting(): AbstractIntSetting = ForcedVbiFrequencySetting

    fun eagerHeartbeatSetting() = vrBooleanSetting("EagerHeartbeat", false)

    fun referenceSpaceModeSetting() = vrIntSetting("ReferenceSpaceMode", 0)

    fun trackingModeSetting() = vrIntSetting("TrackingMode", 0)

    fun unitsPerMeterSetting(): AbstractFloatSetting = FloatSetting.GFX_VR_UNITS_PER_METER

    fun enableLeanBackAngleSetting() = vrBooleanSetting("EnableLeanBackAngle", true)

    fun leanBackAngleSetting() = vrFloatSetting("LeanBackAngle", 0.0f)

    fun enableCameraForwardSetting() = vrBooleanSetting("EnableCameraForward", true)

    fun cameraForwardSetting() = vrFloatSetting("CameraForward", 0.0f)

    fun enableCameraHeightSetting() = vrBooleanSetting("EnableCameraHeight", true)

    fun cameraHeightSetting() = vrFloatSetting("CameraHeight", 0.0f)

    fun enableCameraAnchorSetting() = vrBooleanSetting("EnableCameraAnchor", false)

    fun cameraAnchorSmoothingSetting() = vrFloatSetting("CameraAnchorSmoothing", 0.85f)

    fun enableControllerAnchorSetting() = vrBooleanSetting("EnableControllerAnchor", false)

    fun detectSkyboxSetting() = vrBooleanSetting("DetectSkybox", false)

    fun passthroughSetting() = vrBooleanSetting("Passthrough", false)

    fun passthroughRevealUnrenderedSetting() =
        vrBooleanSetting("PassthroughRemoveBlackBackground", true)

    fun passthroughRemoveBlackClearsSetting() =
        vrBooleanSetting("PassthroughRemoveBlackEFBClears", true)

    fun passthroughSceneOpacitySetting() = vrFloatSetting("PassthroughSceneOpacity", 1.0f)

    fun passthroughCoverageModeSetting() = vrIntSetting("PassthroughCoverageMode", 0)

    fun vrGammaSetting() = vrFloatSetting("Gamma", 1.0f)

    fun exactScreenDepthSetting() = vrBooleanSetting("ExactScreenDepth", true)

    fun autoNativeEfbEffectsSetting() = vrBooleanSetting("AutoNativeEfbEffects", true)

    fun hudThicknessSetting() = vrFloatSetting("HudThickness", 0.0f)

    fun removeBarsSetting() = vrBooleanSetting("RemoveCinematicBars", true)

    fun frameSizeFromXfbSetting() = vrBooleanSetting("FrameSizeFromXFB", true)

    fun panesOnScreenSetting() = vrBooleanSetting("SmallViewportsOnScreen", true)

    fun detectRenderTargetsSetting() = vrBooleanSetting("DetectRenderTargets", false)

    fun orthoScissorFixSetting() = vrBooleanSetting("OrthoScissorFix", true)

    // The PC VRPane exposes one checkbox that drives both the Vulkan and the D3D palette key.
    fun layeredPaletteConversionSetting(): AbstractBooleanSetting = LayeredPaletteConversionSetting

    fun useVulkanMultiviewSetting() = vrBooleanSetting("UseVulkanMultiview", true)

    fun androidDirectToHmdSetting() = vrBooleanSetting("AndroidDirectToHMD", true)

    fun cpuLevel5HintSetting() = vrBooleanSetting("QuestCpuLevel5Hint", false)

    // Defaults must match GraphicsSettings.cpp (Android values).
    fun resolutionScaleSetting() = vrFloatSetting("ResolutionScale", 0.85f)

    fun foveationLevelSetting() = vrIntSetting("FoveationLevel", 2)

    fun dynamicFoveationSetting() = vrBooleanSetting("DynamicFoveation", true)

    // Off by default: forces binned rendering, which hurts EFB-copy-heavy games.
    fun foveateEfbSetting() = vrBooleanSetting("FoveateEFB", false)

    fun virtualScreenSetting() = vrBooleanSetting("VirtualScreen", false)

    fun screenDistanceSetting() = vrFloatSetting("ScreenDistance", 1.5f)

    fun screenSizeSetting() = vrFloatSetting("ScreenSize", 1.5f)

    fun headLockedCurvatureSetting() = vrFloatSetting("HeadLockedCurvature", 0.0f)

    fun dontClearScreenSetting() = vrBooleanSetting("DontClearScreen", false)

    fun disableCpuCullSetting() = vrBooleanSetting("DisableCPUCull", false)

    fun clearEfbCopiesSetting() = vrIntSetting("ClearEFBCopies", 0)

    fun loadCustomShadersSetting() = vrBooleanSetting("LoadCustomShaders", false)


    /**
     * AutoVBIFromHMD is the legacy boolean that ForcedVBIFrequency replaced (SConfig::LoadSettings
     * migrates it to 90 Hz on startup). While it is set it pins the effective rate to 90 Hz even
     * when the frequency reads "Off", so clear it whenever an explicit choice is made — same as the
     * PC VRPane does.
     */
    private object ForcedVbiFrequencySetting : AbstractIntSetting {
        private val backing = vrIntSetting("ForcedVBIFrequency", 0)

        override val isOverridden: Boolean
            get() = backing.isOverridden

        override val isRuntimeEditable: Boolean
            get() = backing.isRuntimeEditable

        override fun delete(settings: Settings): Boolean = backing.delete(settings)

        override val int: Int
            get() = backing.int

        override fun setInt(settings: Settings, newValue: Int) {
            backing.setInt(settings, newValue)
            autoVbiFromHmdSetting().setBoolean(settings, false)
        }
    }

    /**
     * The layered palette conversion path has a separate key per backend family; the PC VRPane
     * keeps them in sync behind a single checkbox, so do the same here.
     */
    private object LayeredPaletteConversionSetting : AbstractBooleanSetting {
        private val vulkanKey = vrBooleanSetting("MetroidThermalVisorFix", true)
        private val d3dKey = vrBooleanSetting("MetroidD3DThermalPaletteFix", true)

        override val isOverridden: Boolean
            get() = vulkanKey.isOverridden

        override val isRuntimeEditable: Boolean
            get() = vulkanKey.isRuntimeEditable

        override fun delete(settings: Settings): Boolean {
            val deletedD3d = d3dKey.delete(settings)
            return vulkanKey.delete(settings) || deletedD3d
        }

        override val boolean: Boolean
            get() = vulkanKey.boolean

        override fun setBoolean(settings: Settings, newValue: Boolean) {
            vulkanKey.setBoolean(settings, newValue)
            d3dKey.setBoolean(settings, newValue)
        }
    }

    private fun openXrRuntimeSetting() = vrBooleanSetting("EnableOpenXR", false)

    private fun perfDefaultsAppliedSetting() = androidBooleanSetting("QuestPerfProfileApplied", false)

    private fun backendMultithreadingReenabledSetting() =
        androidBooleanSetting("QuestBackendMultithreadingReenabled", false)

    private fun controllerSetupAskedSetting() =
        androidBooleanSetting("QuestControllerSetupAsked", false)

    fun shouldShowMirrorSurface(): Boolean {
        if (!BuildConfig.IS_QUEST) {
            return true
        }

        // Flat-in-VR is still an immersive OpenXR session (no 2D Android surface), so key the
        // mirror surface off whether we launch immersively, not off stereo vs. flat.
        return showMirrorSurfaceSetting().boolean || !isOpenXrImmersiveEnabled()
    }

    // True when we launch an immersive OpenXR session at all (either stereo 3D or flat panel).
    fun isOpenXrImmersiveEnabled(): Boolean {
        return BuildConfig.IS_QUEST && openXrEnabledSetting().boolean
    }

    fun isLaunchInVrEnabled(): Boolean {
        return BuildConfig.IS_QUEST &&
            openXrEnabledSetting().boolean &&
            launchInVrSetting().boolean
    }

    fun applyRecommendedDefaults(settings: Settings) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        StringSetting.MAIN_GFX_BACKEND.setString(settings, "Vulkan")
        BooleanSetting.GFX_BACKEND_MULTITHREADING.setBoolean(settings, true)
        IntSetting.GFX_EFB_SCALE.setInt(settings, 4)
        BooleanSetting.GFX_WAIT_FOR_SHADERS_BEFORE_STARTING.setBoolean(settings, false)
        BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(settings, false)
        exactScreenDepthSetting().setBoolean(settings, true)
        androidDirectToHmdSetting().setBoolean(settings, true)
        removeBarsSetting().setBoolean(settings, true)
        virtualScreenSetting().setBoolean(settings, false)
        BooleanSetting.GFX_HACK_IMMEDIATE_XFB.setBoolean(settings, true)
        BooleanSetting.GFX_HACK_VI_SKIP.setBoolean(settings, false)
        perfDefaultsAppliedSetting().setBoolean(settings, true)
        backendMultithreadingReenabledSetting().setBoolean(settings, true)
    }

    /**
     * Restores every setting on the OpenXR screen to its built-in default, mirroring the PC
     * VRPane's "Reset VR Settings" button.
     *
     * Deletes the keys instead of writing values so the platform's compiled-in defaults apply —
     * several of them differ on Android (e.g. ResolutionScale 0.85, FoveationLevel 2,
     * AndroidDirectToHMD on). Non-VR graphics settings (backend, EFB scale) and controller
     * mappings are deliberately left alone; they do not belong to this screen.
     */
    fun resetOpenXrSettings(settings: Settings) {
        val resettable: List<AbstractSetting> = listOf(
            // Runtime
            openXrEnabledSetting(),
            launchInVrSetting(),
            flatScreenSetting(),
            recenterOnLaunchSetting(),
            unitsPerMeterSetting(),
            // Camera
            enableLeanBackAngleSetting(),
            leanBackAngleSetting(),
            enableCameraForwardSetting(),
            cameraForwardSetting(),
            enableCameraHeightSetting(),
            cameraHeightSetting(),
            enableCameraAnchorSetting(),
            cameraAnchorSmoothingSetting(),
            enableControllerAnchorSetting(),
            // Virtual screen
            virtualScreenSetting(),
            exactScreenDepthSetting(),
            autoNativeEfbEffectsSetting(),
            screenDistanceSetting(),
            screenSizeSetting(),
            hudThicknessSetting(),
            headLockedCurvatureSetting(),
            // Rendering
            resolutionScaleSetting(),
            foveationLevelSetting(),
            dynamicFoveationSetting(),
            foveateEfbSetting(),
            clearEfbCopiesSetting(),
            vrGammaSetting(),
            // Framerate
            forcedVbiFrequencySetting(),
            autoVbiFromHmdSetting(),
            eagerHeartbeatSetting(),
            // VR hacks
            useVulkanMultiviewSetting(),
            dontClearScreenSetting(),
            disableCpuCullSetting(),
            removeBarsSetting(),
            frameSizeFromXfbSetting(),
            panesOnScreenSetting(),
            detectRenderTargetsSetting(),
            orthoScissorFixSetting(),
            detectSkyboxSetting(),
            layeredPaletteConversionSetting(),
            // Passthrough
            passthroughSetting(),
            passthroughRevealUnrenderedSetting(),
            passthroughRemoveBlackClearsSetting(),
            passthroughSceneOpacitySetting(),
            passthroughCoverageModeSetting(),
            // Comfort and debug
            leftHandedSetting(),
            showMirrorSurfaceSetting(),
            androidDirectToHmdSetting(),
            cpuLevel5HintSetting(),
            loadCustomShadersSetting(),
            referenceSpaceModeSetting(),
            trackingModeSetting()
        )

        resettable.forEach { it.delete(settings) }
    }

    fun shouldAskAboutControllerSetup(): Boolean {
        return BuildConfig.IS_QUEST && !controllerSetupAskedSetting().boolean
    }

    fun recordControllerSetupChoice(settings: Settings, applyDefaults: Boolean) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        if (applyDefaults) {
            applyDefaultControllerSetup(settings)
        }
        controllerSetupAskedSetting().setBoolean(settings, true)
    }

    /**
     * Points [controller] at the stock Quest OpenXR profile named [profileName].
     *
     * Also used by the mapping screen's "Default" action: Dolphin's built-in defaults target a
     * touchscreen or physical pad that does not exist on Quest, so the OpenXR profile is the only
     * meaningful default there.
     *
     * @return false on non-Quest builds, so callers can fall back to Dolphin's own defaults.
     */
    fun loadStockProfile(controller: EmulatedController, profileName: String): Boolean {
        if (!BuildConfig.IS_QUEST) {
            return false
        }

        controller.loadProfile(controller.getSysProfileDirectoryPath() + profileName)
        controller.setDefaultDevice(OPENXR_DEVICE)
        return true
    }

    /**
     * Installs the stock Quest mappings without making the user choose between Wii and GameCube.
     * The appropriate controller will be used automatically for each emulated console.
     */
    fun applyDefaultControllerSetup(settings: Settings) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        loadStockProfile(EmulatedController.getGcPad(0), GC_PROFILE_NAME)
        loadStockProfile(EmulatedController.getWiimote(0), WIIMOTE_PROFILE_NAME)
        loadStockProfile(EmulatedController.getHotkeys(), HOTKEY_PROFILE_NAME)

        // 6 is an emulated Standard Controller and 3 is an OpenXR Wii Remote.
        IntSetting.MAIN_SI_DEVICE_0.setInt(settings, 6)
        IntSetting.WIIMOTE_1_SOURCE.setInt(settings, 3)
    }

    fun prepareLaunchSettings(settings: Settings) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        if (!perfDefaultsAppliedSetting().boolean) {
            applyRecommendedDefaults(settings)
        }

        StringSetting.MAIN_GFX_BACKEND.setString(settings, "Vulkan")

        // The OpenXR immersive session runs whenever the runtime is enabled. "Launch games in VR"
        // then chooses stereo 3D (StereoMode::OpenXR) vs. a flat mono panel in the VR scene.
        val immersive = isOpenXrImmersiveEnabled()
        val stereo = isLaunchInVrEnabled()
        openXrRuntimeSetting().setBoolean(settings, immersive)
        flatScreenSetting().setBoolean(settings, immersive && !stereo)

        if (immersive) {
            // Native side maps EnableOpenXR + FlatScreen to the stereo mode; keep GFX_STEREO_MODE
            // set to OpenXR only for the stereo path so a stale value can't force stereo in flat.
            IntSetting.GFX_STEREO_MODE.setInt(settings, if (stereo) STEREO_MODE_OPENXR else 0)
            if (!backendMultithreadingReenabledSetting().boolean) {
                BooleanSetting.GFX_BACKEND_MULTITHREADING.setBoolean(settings, true)
                backendMultithreadingReenabledSetting().setBoolean(settings, true)
            }
            BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(settings, false)
            // Immediately Present XFB is left as the user set it (applyRecommendedDefaults
            // turns it on): games that need it off now render correctly in VR, and the
            // head-pose lock is applied automatically whenever it is off.
            BooleanSetting.GFX_HACK_VI_SKIP.setBoolean(settings, false)
        } else {
            // flatScreen already set false above (immersive is false here).
            if (IntSetting.GFX_STEREO_MODE.int == STEREO_MODE_OPENXR) {
                IntSetting.GFX_STEREO_MODE.setInt(settings, 0)
            }
        }

        // Controller profiles are configured explicitly by the first-launch prompt or settings UI.
        // Do not overwrite a user's controller selection whenever a game starts.
    }
}
