// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.ui

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.text.TextUtils
import androidx.appcompat.app.AppCompatActivity
import androidx.collection.ArraySet
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.OpenXRControllerMapperActivity
import org.dolphinemu.dolphinemu.activities.UserDataActivity
import org.dolphinemu.dolphinemu.features.input.model.ControlGroupEnabledSetting
import org.dolphinemu.dolphinemu.features.input.model.InputMappingBooleanSetting
import org.dolphinemu.dolphinemu.features.input.model.InputMappingDoubleSetting
import org.dolphinemu.dolphinemu.features.input.model.InputMappingIntSetting
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.ControlGroup
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.ControlGroupContainer
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.EmulatedController
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.NumericSetting
import org.dolphinemu.dolphinemu.features.input.model.view.InputDeviceSetting
import org.dolphinemu.dolphinemu.features.input.model.view.InputMappingControlSetting
import org.dolphinemu.dolphinemu.features.input.ui.ProfileDialog
import org.dolphinemu.dolphinemu.features.input.ui.ProfileDialogPresenter
import org.dolphinemu.dolphinemu.features.settings.model.*
import org.dolphinemu.dolphinemu.features.settings.model.view.*
import org.dolphinemu.dolphinemu.features.settings.model.AchievementModel.logout
import org.dolphinemu.dolphinemu.model.GpuDriverMetadata
import org.dolphinemu.dolphinemu.utils.*
import kotlin.collections.ArrayList
import kotlin.math.ceil
import kotlin.math.floor

class SettingsFragmentPresenter(
    private val fragmentView: SettingsFragmentView,
    private val context: Context
) {
    private lateinit var menuTag: MenuTag
    private var gameId: String? = null
    private var revision = 0

    private var settingsList: ArrayList<SettingsItem>? = null
    private var hasOldControllerSettings = false

    private var serialPort1Type = 0
    private var controllerNumber = 0
    private var controllerType = 0

    var gpuDriver: GpuDriverMetadata? = null
    private val libNameSetting: StringSetting = StringSetting.GFX_DRIVER_LIB_NAME

    fun onCreate(menuTag: MenuTag, gameId: String?, extras: Bundle) {
        this.gameId = gameId
        this.menuTag = menuTag
        this.revision = extras.getInt(ARG_REVISION, 0)

        if (menuTag.isGCPadMenu || menuTag.isWiimoteExtensionMenu) {
            controllerNumber = menuTag.subType
            controllerType = extras.getInt(ARG_CONTROLLER_TYPE)
        } else if (menuTag.isWiimoteMenu || menuTag.isWiimoteSubmenu) {
            controllerNumber = menuTag.subType
        } else if (menuTag.isSerialPort1Menu) {
            serialPort1Type = extras.getInt(ARG_SERIALPORT1_TYPE)
        } else if (
            menuTag == MenuTag.GRAPHICS
            && this.gameId.isNullOrEmpty()
            && NativeLibrary.IsUninitialized()
            && GpuDriverHelper.supportsCustomDriverLoading()
        ) {
            this.gpuDriver =
                GpuDriverHelper.getInstalledDriverMetadata()
                    ?: GpuDriverHelper.getSystemDriverMetadata(context.applicationContext)
        }
    }

    fun onViewCreated(menuTag: MenuTag, settings: Settings?) {
        this.menuTag = menuTag

        if (!TextUtils.isEmpty(gameId)) {
            fragmentView.fragmentActivity.title = context.getString(R.string.game_settings, gameId)
        }

        this.settings = settings
    }

    var settings: Settings? = null
        set(settings) {
            field = settings
            if (settingsList == null && settings != null) {
                loadSettingsList()
            } else {
                fragmentView.showSettingsList(settingsList!!)
                fragmentView.setOldControllerSettingsWarningVisibility(hasOldControllerSettings)
            }
        }

    fun loadSettingsList() {
        val sl = ArrayList<SettingsItem>()
        when (menuTag) {
            MenuTag.SETTINGS -> addTopLevelSettings(sl)
            MenuTag.CONFIG -> addConfigSettings(sl)
            MenuTag.CONFIG_GENERAL -> addGeneralSettings(sl)
            MenuTag.CONFIG_INTERFACE -> addInterfaceSettings(sl)
            MenuTag.CONFIG_AUDIO -> addAudioSettings(sl)
            MenuTag.CONFIG_PATHS -> addPathsSettings(sl)
            MenuTag.CONFIG_GAME_CUBE -> addGameCubeSettings(sl)
            MenuTag.CONFIG_WII -> addWiiSettings(sl)
            MenuTag.CONFIG_ACHIEVEMENTS -> addAchievementSettings(sl);
            MenuTag.CONFIG_ADVANCED -> addAdvancedSettings(sl)
            MenuTag.GRAPHICS -> addGraphicsSettings(sl)
            MenuTag.CONFIG_SERIALPORT1 -> addSerialPortSubSettings(sl, serialPort1Type)
            MenuTag.GCPAD_TYPE -> addGcPadSettings(sl)
            MenuTag.WIIMOTE -> addWiimoteSettings(sl)
            MenuTag.ENHANCEMENTS -> addEnhanceSettings(sl)
            MenuTag.COLOR_CORRECTION -> addColorCorrectionSettings(sl)
            MenuTag.OPENXR -> addOpenXrSettings(sl)
            MenuTag.STEREOSCOPY -> addStereoSettings(sl)
            MenuTag.HACKS -> addHackSettings(sl)
            MenuTag.STATISTICS -> addStatisticsSettings(sl)
            MenuTag.ADVANCED_GRAPHICS -> addAdvancedGraphicsSettings(sl)
            MenuTag.QUEST_HIDE_OBJECTS -> addQuestOverrideSettings(
                sl,
                QuestGameOverrideSettings.Category.HIDE_OBJECTS
            )
            MenuTag.QUEST_SHADER_OVERRIDES -> addQuestOverrideSettings(
                sl,
                QuestGameOverrideSettings.Category.SHADER_OVERRIDES
            )
            MenuTag.QUEST_ELEMENTS_GROUP_OVERRIDES -> addQuestOverrideSettings(
                sl,
                QuestGameOverrideSettings.Category.ELEMENTS_GROUP_OVERRIDES
            )
            MenuTag.QUEST_TEXTURE_ELEMENT_OVERRIDES -> addQuestOverrideSettings(
                sl,
                QuestGameOverrideSettings.Category.TEXTURE_ELEMENT_OVERRIDES
            )
            MenuTag.QUEST_VR_CAMERA -> addQuestVrCameraSettings(sl)
            MenuTag.QUEST_VR_VIRTUAL_SCREEN -> addQuestVrVirtualScreenSettings(sl)
            MenuTag.QUEST_VR_RENDERING -> addQuestVrRenderingSettings(sl)
            MenuTag.QUEST_VR_FRAMERATE -> addQuestVrFramerateSettings(sl)
            MenuTag.QUEST_VR_HACKS -> addQuestVrHackSettings(sl)
            MenuTag.QUEST_VR_PASSTHROUGH -> addQuestVrPassthroughSettings(sl)
            MenuTag.QUEST_VR_DEBUG -> addQuestVrDebugSettings(sl)
            MenuTag.QUEST_VR_CONFIG -> addQuestGameVrConfigSettings(sl)
            MenuTag.QUEST_VR_CONFIG_CAMERA -> addQuestGameVrCameraSettings(sl)
            MenuTag.QUEST_VR_CONFIG_VIRTUAL_SCREEN -> addQuestGameVrVirtualScreenSettings(sl)
            MenuTag.QUEST_VR_CONFIG_RENDERING -> addQuestGameVrRenderingSettings(sl)
            MenuTag.QUEST_VR_CONFIG_FRAMERATE -> addQuestGameVrFramerateSettings(sl)
            MenuTag.QUEST_VR_CONFIG_HACKS -> addQuestGameVrHackSettings(sl)
            MenuTag.QUEST_VR_CONFIG_PASSTHROUGH -> addQuestGameVrPassthroughSettings(sl)
            MenuTag.QUEST_VR_CONFIG_DEBUG -> addQuestGameVrDebugSettings(sl)
            MenuTag.CONFIG_LOG -> addLogConfigurationSettings(sl)
            MenuTag.DEBUG -> addDebugSettings(sl)
            MenuTag.GCPAD_1,
            MenuTag.GCPAD_2,
            MenuTag.GCPAD_3,
            MenuTag.GCPAD_4 -> addGcPadSubSettings(
                sl,
                controllerNumber,
                controllerType
            )

            MenuTag.WIIMOTE_1,
            MenuTag.WIIMOTE_2,
            MenuTag.WIIMOTE_3,
            MenuTag.WIIMOTE_4 -> addWiimoteSubSettings(
                sl,
                controllerNumber
            )

            MenuTag.WIIMOTE_EXTENSION_1,
            MenuTag.WIIMOTE_EXTENSION_2,
            MenuTag.WIIMOTE_EXTENSION_3,
            MenuTag.WIIMOTE_EXTENSION_4 -> addExtensionTypeSettings(
                sl,
                controllerNumber,
                controllerType
            )

            MenuTag.WIIMOTE_GENERAL_1,
            MenuTag.WIIMOTE_GENERAL_2,
            MenuTag.WIIMOTE_GENERAL_3,
            MenuTag.WIIMOTE_GENERAL_4 -> addWiimoteGeneralSubSettings(
                sl,
                controllerNumber
            )

            MenuTag.WIIMOTE_MOTION_SIMULATION_1,
            MenuTag.WIIMOTE_MOTION_SIMULATION_2,
            MenuTag.WIIMOTE_MOTION_SIMULATION_3,
            MenuTag.WIIMOTE_MOTION_SIMULATION_4 -> addWiimoteMotionSimulationSubSettings(
                sl,
                controllerNumber
            )

            MenuTag.WIIMOTE_MOTION_INPUT_1,
            MenuTag.WIIMOTE_MOTION_INPUT_2,
            MenuTag.WIIMOTE_MOTION_INPUT_3,
            MenuTag.WIIMOTE_MOTION_INPUT_4 -> addWiimoteMotionInputSubSettings(
                sl,
                controllerNumber
            )

            MenuTag.HOTKEYS -> addHotkeySettings(sl)

            MenuTag.HOTKEYS_GENERAL,
            MenuTag.HOTKEYS_TAS,
            MenuTag.HOTKEYS_DEBUGGING,
            MenuTag.HOTKEYS_WII,
            MenuTag.HOTKEYS_CONTROLLER_PROFILE,
            MenuTag.HOTKEYS_GRAPHICS,
            MenuTag.HOTKEYS_VR,
            MenuTag.HOTKEYS_3D,
            MenuTag.HOTKEYS_SAVE_STATES,
            MenuTag.HOTKEYS_STATES_OTHER,
            MenuTag.HOTKEYS_GBA,
            MenuTag.HOTKEYS_USB,
            MenuTag.HOTKEYS_ANDROID -> addHotkeyCategorySettings(sl, menuTag)

            else -> throw UnsupportedOperationException("Unimplemented menu")
        }

        settingsList = sl
        fragmentView.showSettingsList(settingsList!!)
    }

    private fun addTopLevelSettings(sl: ArrayList<SettingsItem>) {
        sl.add(SubmenuSetting(context, R.string.config, MenuTag.CONFIG))
        sl.add(SubmenuSetting(context, R.string.graphics_settings, MenuTag.GRAPHICS))
        if (BuildConfig.IS_QUEST && gameId.isNullOrEmpty()) {
            sl.add(SubmenuSetting(context, R.string.openxr_submenu, MenuTag.OPENXR))
        }
        if (BuildConfig.IS_QUEST && !gameId.isNullOrEmpty()) {
            sl.add(SubmenuSetting(context, R.string.quest_vr_config, MenuTag.QUEST_VR_CONFIG))
            sl.add(SubmenuSetting(context, R.string.quest_hide_objects, MenuTag.QUEST_HIDE_OBJECTS))
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.quest_shader_overrides,
                    MenuTag.QUEST_SHADER_OVERRIDES
                )
            )
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.quest_elements_group_overrides,
                    MenuTag.QUEST_ELEMENTS_GROUP_OVERRIDES
                )
            )
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.quest_texture_element_overrides,
                    MenuTag.QUEST_TEXTURE_ELEMENT_OVERRIDES
                )
            )
        }

        sl.add(SubmenuSetting(context, R.string.gcpad_settings, MenuTag.GCPAD_TYPE))
        if (settings!!.isWii) {
            sl.add(SubmenuSetting(context, R.string.wiimote_settings, MenuTag.WIIMOTE))
        }
        sl.add(SubmenuSetting(context, R.string.hotkey_settings, MenuTag.HOTKEYS))

        sl.add(HeaderSetting(context, R.string.setting_clear_info, 0))
    }

    private fun addConfigSettings(sl: ArrayList<SettingsItem>) {
        sl.add(SubmenuSetting(context, R.string.general_submenu, MenuTag.CONFIG_GENERAL))
        sl.add(SubmenuSetting(context, R.string.interface_submenu, MenuTag.CONFIG_INTERFACE))
        sl.add(SubmenuSetting(context, R.string.audio_submenu, MenuTag.CONFIG_AUDIO))
        sl.add(SubmenuSetting(context, R.string.paths_submenu, MenuTag.CONFIG_PATHS))
        sl.add(SubmenuSetting(context, R.string.gamecube_submenu, MenuTag.CONFIG_GAME_CUBE))
        sl.add(SubmenuSetting(context, R.string.wii_submenu, MenuTag.CONFIG_WII))
        sl.add(SubmenuSetting(context, R.string.achievements_submenu, MenuTag.CONFIG_ACHIEVEMENTS))
        sl.add(SubmenuSetting(context, R.string.advanced_submenu, MenuTag.CONFIG_ADVANCED))
        sl.add(SubmenuSetting(context, R.string.log_submenu, MenuTag.CONFIG_LOG))
        sl.add(SubmenuSetting(context, R.string.debug_submenu, MenuTag.DEBUG))
        sl.add(
            RunRunnable(context, R.string.user_data_submenu, 0, 0, 0, false)
            { UserDataActivity.launch(context) }
        )
    }

    private fun addGeneralSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_CPU_THREAD,
                R.string.dual_core,
                R.string.dual_core_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_ENABLE_CHEATS,
                R.string.enable_cheats,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_OVERRIDE_REGION_SETTINGS,
                R.string.override_region_settings,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_AUTO_DISC_CHANGE,
                R.string.auto_disc_change,
                0
            )
        )
        sl.add(
            PercentSliderSetting(
                context,
                FloatSetting.MAIN_EMULATION_SPEED,
                R.string.speed_limit,
                0,
                if (AchievementModel.isHardcoreModeActive()) 100f else 0f,
                200f,
                "%",
                1f,
                false
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_FALLBACK_REGION,
                R.string.fallback_region,
                0,
                R.array.regionEntries,
                R.array.regionValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_ANALYTICS_ENABLED,
                R.string.analytics,
                0
            )
        )
        sl.add(
            RunRunnable(
                context,
                R.string.analytics_new_id,
                0,
                R.string.analytics_new_id_confirmation,
                0,
                true
            ) { NativeLibrary.GenerateNewStatisticsId() }
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_ENABLE_SAVESTATES,
                R.string.enable_save_states,
                R.string.enable_save_states_description
            )
        )
    }

    private fun addInterfaceSettings(sl: ArrayList<SettingsItem>) {
        // Hide the orientation setting if the device only supports one orientation. Old devices which
        // support both portrait and landscape may report support for neither, so we use ==, not &&.
        val packageManager = context.packageManager
        if (packageManager.hasSystemFeature(PackageManager.FEATURE_SCREEN_PORTRAIT) ==
            packageManager.hasSystemFeature(PackageManager.FEATURE_SCREEN_LANDSCAPE)
        ) {
            sl.add(
                SingleChoiceSetting(
                    context,
                    IntSetting.MAIN_EMULATION_ORIENTATION,
                    R.string.emulation_screen_orientation,
                    0,
                    R.array.orientationEntries,
                    R.array.orientationValues
                )
            )
        }

        // Only android 9+ supports this feature.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.MAIN_EXPAND_TO_CUTOUT_AREA,
                    R.string.expand_to_cutout_area,
                    R.string.expand_to_cutout_area_description
                )
            )
        }

        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_USE_PANIC_HANDLERS,
                R.string.panic_handlers,
                R.string.panic_handlers_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_OSD_MESSAGES,
                R.string.osd_messages,
                R.string.osd_messages_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_TIME_TRACKING,
                R.string.time_tracking,
                R.string.time_tracking_description
            )
        )

        val appTheme: AbstractIntSetting = object : AbstractIntSetting {
            override val isOverridden: Boolean
                get() = IntSetting.MAIN_INTERFACE_THEME.isOverridden

            // This only affects app UI
            override val isRuntimeEditable: Boolean = true

            override fun delete(settings: Settings): Boolean {
                ThemeHelper.deleteThemeKey((fragmentView.fragmentActivity as AppCompatActivity))
                return IntSetting.MAIN_INTERFACE_THEME.delete(settings)
            }

            override val int: Int
                get() = IntSetting.MAIN_INTERFACE_THEME.int

            override fun setInt(settings: Settings, newValue: Int) {
                IntSetting.MAIN_INTERFACE_THEME.setInt(settings, newValue)
                ThemeHelper.saveTheme(
                    (fragmentView.fragmentActivity as AppCompatActivity),
                    newValue
                )
            }
        }

        // If a Monet theme is run on a device below API 31, the app will crash
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            sl.add(
                SingleChoiceSetting(
                    context,
                    appTheme,
                    R.string.change_theme,
                    0,
                    R.array.themeEntriesA12,
                    R.array.themeValuesA12
                )
            )
        } else {
            sl.add(
                SingleChoiceSetting(
                    context,
                    appTheme,
                    R.string.change_theme,
                    0,
                    R.array.themeEntries,
                    R.array.themeValues
                )
            )
        }

        val themeMode: AbstractIntSetting = object : AbstractIntSetting {
            override val isOverridden: Boolean
                get() = IntSetting.MAIN_INTERFACE_THEME_MODE.isOverridden

            // This only affects app UI
            override val isRuntimeEditable: Boolean = true

            override fun delete(settings: Settings): Boolean {
                ThemeHelper.deleteThemeModeKey((fragmentView.fragmentActivity as AppCompatActivity))
                return IntSetting.MAIN_INTERFACE_THEME_MODE.delete(settings)
            }

            override val int: Int
                get() = IntSetting.MAIN_INTERFACE_THEME_MODE.int

            override fun setInt(settings: Settings, newValue: Int) {
                IntSetting.MAIN_INTERFACE_THEME_MODE.setInt(settings, newValue)
                ThemeHelper.saveThemeMode(
                    (fragmentView.fragmentActivity as AppCompatActivity),
                    newValue
                )
            }
        }

        sl.add(
            SingleChoiceSetting(
                context,
                themeMode,
                R.string.change_theme_mode,
                0,
                R.array.themeModeEntries,
                R.array.themeModeValues
            )
        )

        val blackBackgrounds: AbstractBooleanSetting = object : AbstractBooleanSetting {
            override val isOverridden: Boolean
                get() = BooleanSetting.MAIN_USE_BLACK_BACKGROUNDS.isOverridden

            override val isRuntimeEditable: Boolean = true

            override fun delete(settings: Settings): Boolean {
                ThemeHelper.deleteBackgroundSetting((fragmentView.fragmentActivity as AppCompatActivity))
                return BooleanSetting.MAIN_USE_BLACK_BACKGROUNDS.delete(settings)
            }

            override val boolean: Boolean
                get() = BooleanSetting.MAIN_USE_BLACK_BACKGROUNDS.boolean

            override fun setBoolean(settings: Settings, newValue: Boolean) {
                BooleanSetting.MAIN_USE_BLACK_BACKGROUNDS.setBoolean(settings, newValue)
                ThemeHelper.saveBackgroundSetting(
                    (fragmentView.fragmentActivity as AppCompatActivity),
                    newValue
                )
            }
        }

        sl.add(
            SwitchSetting(
                context,
                blackBackgrounds,
                R.string.use_black_backgrounds,
                R.string.use_black_backgrounds_description
            )
        )
    }

    private fun addAudioSettings(sl: ArrayList<SettingsItem>) {
        val DSP_HLE = 0
        val DSP_LLE_RECOMPILER = 1
        val DSP_LLE_INTERPRETER = 2

        val dspEmulationEngine: AbstractIntSetting = object : AbstractIntSetting {
            override val int: Int
                get() = if (BooleanSetting.MAIN_DSP_HLE.boolean) {
                    DSP_HLE
                } else {
                    val jit = BooleanSetting.MAIN_DSP_JIT.boolean
                    if (jit) DSP_LLE_RECOMPILER else DSP_LLE_INTERPRETER
                }

            override fun setInt(settings: Settings, newValue: Int) {
                when (newValue) {
                    DSP_HLE -> {
                        BooleanSetting.MAIN_DSP_HLE.setBoolean(settings, true)
                        BooleanSetting.MAIN_DSP_JIT.setBoolean(settings, true)
                    }

                    DSP_LLE_RECOMPILER -> {
                        BooleanSetting.MAIN_DSP_HLE.setBoolean(settings, false)
                        BooleanSetting.MAIN_DSP_JIT.setBoolean(settings, true)
                    }

                    DSP_LLE_INTERPRETER -> {
                        BooleanSetting.MAIN_DSP_HLE.setBoolean(settings, false)
                        BooleanSetting.MAIN_DSP_JIT.setBoolean(settings, false)
                    }
                }
            }

            override val isOverridden: Boolean
                get() = BooleanSetting.MAIN_DSP_HLE.isOverridden ||
                        BooleanSetting.MAIN_DSP_JIT.isOverridden

            override val isRuntimeEditable: Boolean
                get() = BooleanSetting.MAIN_DSP_HLE.isRuntimeEditable &&
                        BooleanSetting.MAIN_DSP_JIT.isRuntimeEditable

            override fun delete(settings: Settings): Boolean {
                // Not short circuiting
                return BooleanSetting.MAIN_DSP_HLE.delete(settings) and
                        BooleanSetting.MAIN_DSP_JIT.delete(settings)
            }
        }

        // TODO: Exclude values from arrays instead of having multiple arrays.
        val defaultCpuCore = NativeLibrary.DefaultCPUCore()
        val dspEngineEntries: Int
        val dspEngineValues: Int
        if (defaultCpuCore == 1) {
            dspEngineEntries = R.array.dspEngineEntriesX86_64
            dspEngineValues = R.array.dspEngineValuesX86_64
        } else {
            dspEngineEntries = R.array.dspEngineEntriesGeneric
            dspEngineValues = R.array.dspEngineValuesGeneric
        }
        sl.add(
            SingleChoiceSetting(
                context,
                dspEmulationEngine,
                R.string.dsp_emulation_engine,
                0,
                dspEngineEntries,
                dspEngineValues
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.MAIN_AUDIO_BUFFER_SIZE,
                R.string.audio_buffer_size,
                R.string.audio_buffer_size_description,
                16,
                512,
                "ms",
                8
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_AUDIO_FILL_GAPS,
                R.string.audio_fill_gaps,
                R.string.audio_fill_gaps_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_AUDIO_PRESERVE_PITCH,
                R.string.audio_preserve_pitch,
                R.string.audio_preserve_pitch_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.MAIN_AUDIO_VOLUME,
                R.string.audio_volume,
                0,
                0,
                100,
                "%",
                1
            )
        )
    }

    private fun addPathsSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_RECURSIVE_ISO_PATHS,
                R.string.search_subfolders,
                0
            )
        )
        sl.add(
            FilePicker(
                context,
                StringSetting.MAIN_DEFAULT_ISO,
                R.string.default_ISO,
                0,
                fragmentView.activityResultLaunchers.requestGameFile,
                null
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_FS_PATH,
                R.string.wii_NAND_root,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/Wii"
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_DUMP_PATH,
                R.string.dump_path,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/Dump"
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_LOAD_PATH,
                R.string.load_path,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/Load"
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_RESOURCEPACK_PATH,
                R.string.resource_pack_path,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/ResourcePacks"
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_WFS_PATH,
                R.string.wfs_path,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/WFS"
            )
        )
    }

    private fun addGameCubeSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.ipl_settings, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_SKIP_IPL,
                R.string.skip_main_menu,
                R.string.skip_main_menu_description
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_GC_LANGUAGE,
                R.string.system_language,
                0,
                R.array.gameCubeSystemLanguageEntries,
                R.array.gameCubeSystemLanguageValues
            )
        )

        sl.add(HeaderSetting(context, R.string.device_settings, 0))
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SLOT_A,
                R.string.slot_a_device,
                0,
                R.array.slotDeviceEntries,
                R.array.slotDeviceValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SLOT_B,
                R.string.slot_b_device,
                0,
                R.array.slotDeviceEntries,
                R.array.slotDeviceValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SERIAL_PORT_1,
                R.string.serial_port_1_device,
                0,
                R.array.serialPort1DeviceEntries,
                R.array.serialPort1DeviceValues,
                MenuTag.CONFIG_SERIALPORT1
            )
        )
    }

    private fun addWiiSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.wii_misc_settings, 0))
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.SYSCONF_LANGUAGE,
                R.string.system_language,
                0,
                R.array.wiiSystemLanguageEntries,
                R.array.wiiSystemLanguageValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.SYSCONF_WIDESCREEN,
                R.string.wii_widescreen,
                R.string.wii_widescreen_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.SYSCONF_PAL60,
                R.string.wii_pal60,
                R.string.wii_pal60_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.SYSCONF_SCREENSAVER,
                R.string.wii_screensaver,
                R.string.wii_screensaver_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WII_WIILINK_ENABLE,
                R.string.wii_enable_wiilink,
                R.string.wii_enable_wiilink_description
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.SYSCONF_SOUND_MODE,
                R.string.sound_mode,
                0,
                R.array.soundModeEntries,
                R.array.soundModeValues
            )
        )

        sl.add(HeaderSetting(context, R.string.wii_sd_card_settings, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WII_SD_CARD,
                R.string.insert_sd_card,
                R.string.insert_sd_card_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_ALLOW_SD_WRITES,
                R.string.wii_sd_card_allow_writes,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC,
                R.string.wii_sd_card_sync,
                R.string.wii_sd_card_sync_description
            )
        )
        // TODO: Hardcoding "Load" here is wrong, because the user may have changed the Load path.
        // The code structure makes this hard to fix, and with scoped storage active the Load path
        // can't be changed anyway
        sl.add(
            FilePicker(
                context,
                StringSetting.MAIN_WII_SD_CARD_IMAGE_PATH,
                R.string.wii_sd_card_path,
                0,
                fragmentView.activityResultLaunchers.requestRawFile,
                "/Load/WiiSD.raw"
            )
        )
        sl.add(
            DirectoryPicker(
                context,
                StringSetting.MAIN_WII_SD_CARD_SYNC_FOLDER_PATH,
                R.string.wii_sd_sync_folder,
                0,
                fragmentView.activityResultLaunchers.requestDirectory,
                "/Load/WiiSDSync/"
            )
        )
        sl.add(
            RunRunnable(
                context,
                R.string.wii_sd_card_folder_to_file,
                0,
                R.string.wii_sd_card_folder_to_file_confirmation,
                0,
                false
            ) { convertOnThread { WiiUtils.syncSdFolderToSdImage() } }
        )
        sl.add(
            RunRunnable(
                context,
                R.string.wii_sd_card_file_to_folder,
                0,
                R.string.wii_sd_card_file_to_folder_confirmation,
                0,
                false
            ) { convertOnThread { WiiUtils.syncSdImageToSdFolder() } }
        )

        sl.add(HeaderSetting(context, R.string.wii_wiimote_settings, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.SYSCONF_WIIMOTE_MOTOR,
                R.string.wiimote_rumble,
                0
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.SYSCONF_SPEAKER_VOLUME,
                R.string.wiimote_volume,
                0,
                0,
                127,
                "",
                1
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.SYSCONF_SENSOR_BAR_SENSITIVITY,
                R.string.sensor_bar_sensitivity,
                0,
                1,
                5,
                "",
                1
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.SYSCONF_SENSOR_BAR_POSITION,
                R.string.sensor_bar_position,
                0,
                R.array.sensorBarPositionEntries,
                R.array.sensorBarPositionValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WIIMOTE_CONTINUOUS_SCANNING,
                R.string.wiimote_scanning,
                R.string.wiimote_scanning_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WIIMOTE_ENABLE_SPEAKER,
                R.string.wiimote_speaker,
                R.string.wiimote_speaker_description
            )
        )

        sl.add(HeaderSetting(context, R.string.emulated_usb_devices, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_EMULATE_SKYLANDER_PORTAL,
                R.string.emulate_skylander_portal,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_EMULATE_INFINITY_BASE,
                R.string.emulate_infinity_base,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_EMULATE_WII_SPEAK,
                R.string.emulate_wii_speak,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_WII_SPEAK_MUTED,
                R.string.mute_wii_speak,
                0
            )
        )
    }

    private fun addAchievementSettings(sl: ArrayList<SettingsItem>) {
        val achievementsEnabledSetting: AbstractBooleanSetting = object : AbstractBooleanSetting {
            override val boolean: Boolean
                get() = BooleanSetting.ACHIEVEMENTS_ENABLED.boolean

            override fun setBoolean(settings: Settings, newValue: Boolean) {
                BooleanSetting.ACHIEVEMENTS_ENABLED.setBoolean(settings, newValue)
                if (newValue)
                    AchievementModel.init()
                else
                    AchievementModel.shutdown()
                loadSettingsList()
            }

            override val isOverridden: Boolean
                get() = BooleanSetting.ACHIEVEMENTS_ENABLED.isOverridden

            override val isRuntimeEditable: Boolean
                get() = BooleanSetting.ACHIEVEMENTS_ENABLED.isRuntimeEditable

            override fun delete(settings: Settings): Boolean {
                val result = BooleanSetting.ACHIEVEMENTS_ENABLED.delete(settings)
                AchievementModel.shutdown()
                loadSettingsList()
                return result
            }
        }

        sl.add(
            SwitchSetting(
                context,
                achievementsEnabledSetting,
                R.string.achievements_enabled,
                0
            )
        )
        if (BooleanSetting.ACHIEVEMENTS_ENABLED.boolean) {
            if (StringSetting.ACHIEVEMENTS_API_TOKEN.string == "") {
                sl.add(
                    RunRunnable(
                        context,
                        R.string.achievements_login,
                        0,
                        0,
                        0,
                        false
                    ) {
                      fragmentView.showDialogFragment(LoginDialog(this))
                      loadSettingsList()
                    })
            } else {
                sl.add(
                    RunRunnable(
                        context,
                        R.string.achievements_logout,
                        0,
                        0,
                        0,
                        false
                    ) {
                      logout()
                      loadSettingsList()
                    })
            }
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_HARDCORE_ENABLED,
                    R.string.achievements_hardcore_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_UNOFFICIAL_ENABLED,
                    R.string.achievements_unofficial_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_ENCORE_ENABLED,
                    R.string.achievements_encore_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_SPECTATOR_ENABLED,
                    R.string.achievements_spectator_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_LEADERBOARD_TRACKER_ENABLED,
                    R.string.achievements_leaderboard_tracker_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_CHALLENGE_INDICATORS_ENABLED,
                    R.string.achievements_challenge_indicators_enabled,
                    0
                )
            )
            sl.add(
                SwitchSetting(
                    context,
                    BooleanSetting.ACHIEVEMENTS_PROGRESS_ENABLED,
                    R.string.achievements_progress_enabled,
                    0
                )
            )
        }
    }

    private fun addAdvancedSettings(sl: ArrayList<SettingsItem>) {
        val SYNC_GPU_NEVER = 0
        val SYNC_GPU_ON_IDLE_SKIP = 1
        val SYNC_GPU_ALWAYS = 2

        val synchronizeGpuThread: AbstractIntSetting = object : AbstractIntSetting {
            override val int: Int
                get() = if (BooleanSetting.MAIN_SYNC_GPU.boolean) {
                    SYNC_GPU_ALWAYS
                } else {
                    val syncOnSkipIdle = BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.boolean
                    if (syncOnSkipIdle) SYNC_GPU_ON_IDLE_SKIP else SYNC_GPU_NEVER
                }

            override fun setInt(settings: Settings, newValue: Int) {
                when (newValue) {
                    SYNC_GPU_NEVER -> {
                        BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.setBoolean(settings, false)
                        BooleanSetting.MAIN_SYNC_GPU.setBoolean(settings, false)
                    }

                    SYNC_GPU_ON_IDLE_SKIP -> {
                        BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.setBoolean(settings, true)
                        BooleanSetting.MAIN_SYNC_GPU.setBoolean(settings, false)
                    }

                    SYNC_GPU_ALWAYS -> {
                        BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.setBoolean(settings, true)
                        BooleanSetting.MAIN_SYNC_GPU.setBoolean(settings, true)
                    }
                }
            }

            override val isOverridden: Boolean
                get() = BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.isOverridden ||
                        BooleanSetting.MAIN_SYNC_GPU.isOverridden

            override val isRuntimeEditable: Boolean
                get() = BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.isRuntimeEditable &&
                        BooleanSetting.MAIN_SYNC_GPU.isRuntimeEditable

            override fun delete(settings: Settings): Boolean {
                // Not short circuiting
                return BooleanSetting.MAIN_SYNC_ON_SKIP_IDLE.delete(settings) and
                        BooleanSetting.MAIN_SYNC_GPU.delete(settings)
            }
        }

        // TODO: Having different emuCoresEntries/emuCoresValues for each architecture is annoying.
        //       The proper solution would be to have one set of entries and one set of values
        //       and exclude the values that aren't present in PowerPC::AvailableCPUCores().
        val defaultCpuCore = NativeLibrary.DefaultCPUCore()
        val emuCoresEntries: Int
        val emuCoresValues: Int
        when (defaultCpuCore) {
            1 -> {
                emuCoresEntries = R.array.emuCoresEntriesX86_64
                emuCoresValues = R.array.emuCoresValuesX86_64
            }

            4 -> {
                emuCoresEntries = R.array.emuCoresEntriesARM64
                emuCoresValues = R.array.emuCoresValuesARM64
            }

            else -> {
                emuCoresEntries = R.array.emuCoresEntriesGeneric
                emuCoresValues = R.array.emuCoresValuesGeneric
            }
        }

        sl.add(HeaderSetting(context, R.string.cpu_options, 0))
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_CPU_CORE,
                R.string.cpu_core,
                0,
                emuCoresEntries,
                emuCoresValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_MMU,
                R.string.mmu_enable,
                R.string.mmu_enable_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_PAUSE_ON_PANIC,
                R.string.pause_on_panic,
                R.string.pause_on_panic_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_ACCURATE_CPU_CACHE,
                R.string.enable_cpu_cache,
                R.string.enable_cpu_cache_description
            )
        )

        sl.add(HeaderSetting(context, R.string.clock_override, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_OVERCLOCK_ENABLE,
                R.string.overclock_enable,
                R.string.overclock_enable_description
            )
        )
        sl.add(
            PercentSliderSetting(
                context,
                FloatSetting.MAIN_OVERCLOCK,
                R.string.overclock_title,
                R.string.overclock_title_description,
                0f,
                500f,
                "%",
                1f,
                false
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_VI_OVERCLOCK_ENABLE,
                R.string.vi_overclock_enable,
                R.string.vi_overclock_enable_description
            )
        )
        sl.add(
            PercentSliderSetting(
                context,
                FloatSetting.MAIN_VI_OVERCLOCK,
                R.string.vi_overclock_title,
                R.string.vi_overclock_title_description,
                0f,
                500f,
                "%",
                1f,
                false
            )
        )

        val mem1Size = ScaledIntSetting(1024 * 1024, IntSetting.MAIN_MEM1_SIZE)
        val mem2Size = ScaledIntSetting(1024 * 1024, IntSetting.MAIN_MEM2_SIZE)

        sl.add(HeaderSetting(context, R.string.memory_override, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_RAM_OVERRIDE_ENABLE,
                R.string.enable_memory_size_override,
                R.string.enable_memory_size_override_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                mem1Size,
                R.string.main_mem1_size,
                0,
                24,
                64,
                "MB",
                1
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                mem2Size,
                R.string.main_mem2_size,
                0,
                64,
                128,
                "MB",
                1
            )
        )

        sl.add(HeaderSetting(context, R.string.gpu_options, 0))
        sl.add(
            SingleChoiceSetting(
                context,
                synchronizeGpuThread,
                R.string.synchronize_gpu_thread,
                R.string.synchronize_gpu_thread_description,
                R.array.synchronizeGpuThreadEntries,
                R.array.synchronizeGpuThreadValues
            )
        )

        sl.add(HeaderSetting(context, R.string.custom_rtc_options, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_CUSTOM_RTC_ENABLE,
                R.string.custom_rtc_enable,
                R.string.custom_rtc_description
            )
        )
        sl.add(
            DateTimeChoiceSetting(
                context,
                StringSetting.MAIN_CUSTOM_RTC_VALUE,
                R.string.set_custom_rtc,
                0
            )
        )

        sl.add(HeaderSetting(context, R.string.misc_settings, 0))
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.MAIN_FAST_DISC_SPEED,
                R.string.emulate_disc_speed,
                R.string.emulate_disc_speed_description
            )
        )
    }

    private fun addSerialPortSubSettings(sl: ArrayList<SettingsItem>, serialPort1Type: Int) {
        if (serialPort1Type == 6) {
            // Triforce Baseboard
            sl.add(
                InputStringSetting(
                    context,
                    StringSetting.MAIN_TRIFORCE_IP_REDIRECTIONS,
                    R.string.triforce_ip_redirections,
                    0
                )
            )
        } else if (serialPort1Type == 10) {
            // Broadband Adapter (XLink Kai)
            sl.add(HyperLinkHeaderSetting(context, R.string.xlink_kai_guide_header, 0))
            sl.add(
                InputStringSetting(
                    context,
                    StringSetting.MAIN_BBA_XLINK_IP,
                    R.string.xlink_kai_bba_ip,
                    R.string.xlink_kai_bba_ip_description
                )
            )
        } else if (serialPort1Type == 11) {
            // Broadband Adapter (tapserver)
            sl.add(
                InputStringSetting(
                    context,
                    StringSetting.MAIN_BBA_TAPSERVER_DESTINATION,
                    R.string.bba_tapserver_destination,
                    R.string.bba_tapserver_destination_description
                )
            )
        } else if (serialPort1Type == 12) {
            // Broadband Adapter (Built In)
            sl.add(
                InputStringSetting(
                    context,
                    StringSetting.MAIN_BBA_BUILTIN_DNS,
                    R.string.bba_builtin_dns,
                    R.string.bba_builtin_dns_description
                )
            )
        } else if (serialPort1Type == 13) {
            // Modem Adapter (tapserver)
            sl.add(
                InputStringSetting(
                    context,
                    StringSetting.MAIN_MODEM_TAPSERVER_DESTINATION,
                    R.string.modem_tapserver_destination,
                    R.string.modem_tapserver_destination_description
                )
            )
        }
    }

    private fun addGcPadSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SI_DEVICE_0,
                R.string.controller_0,
                0,
                R.array.gcpadTypeEntries,
                R.array.gcpadTypeValues,
                MenuTag.getGCPadMenuTag(0)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SI_DEVICE_1,
                R.string.controller_1,
                0,
                R.array.gcpadTypeEntries,
                R.array.gcpadTypeValues,
                MenuTag.getGCPadMenuTag(1)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SI_DEVICE_2,
                R.string.controller_2,
                0,
                R.array.gcpadTypeEntries,
                R.array.gcpadTypeValues,
                MenuTag.getGCPadMenuTag(2)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.MAIN_SI_DEVICE_3,
                R.string.controller_3,
                0,
                R.array.gcpadTypeEntries,
                R.array.gcpadTypeValues,
                MenuTag.getGCPadMenuTag(3)
            )
        )
    }

    private fun addWiimoteSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.WIIMOTE_1_SOURCE,
                R.string.wiimote_0,
                0,
                R.array.wiimoteTypeEntries,
                R.array.wiimoteTypeValues,
                MenuTag.getWiimoteMenuTag(0)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.WIIMOTE_2_SOURCE,
                R.string.wiimote_1,
                0,
                R.array.wiimoteTypeEntries,
                R.array.wiimoteTypeValues,
                MenuTag.getWiimoteMenuTag(1)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.WIIMOTE_3_SOURCE,
                R.string.wiimote_2,
                0,
                R.array.wiimoteTypeEntries,
                R.array.wiimoteTypeValues,
                MenuTag.getWiimoteMenuTag(2)
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.WIIMOTE_4_SOURCE,
                R.string.wiimote_3,
                0,
                R.array.wiimoteTypeEntries,
                R.array.wiimoteTypeValues,
                MenuTag.getWiimoteMenuTag(3)
            )
        )
        sl.add(SwitchSetting(context, object : AbstractBooleanSetting {
            override val isOverridden: Boolean = IntSetting.WIIMOTE_BB_SOURCE.isOverridden

            override val isRuntimeEditable: Boolean = IntSetting.WIIMOTE_BB_SOURCE.isRuntimeEditable

            override fun delete(settings: Settings): Boolean {
                return IntSetting.WIIMOTE_BB_SOURCE.delete(settings)
            }

            override val boolean: Boolean get() = IntSetting.WIIMOTE_BB_SOURCE.int == 2

            override fun setBoolean(settings: Settings, newValue: Boolean) {
                // 0 == None
                // 1 == Emulated
                // 2 == Real
                IntSetting.WIIMOTE_BB_SOURCE.setInt(settings, if (newValue) 2 else 0)
            }
        }, R.string.real_balance_board, 0))
    }

    private fun addGraphicsSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.graphics_general, 0))
        sl.add(
            StringSingleChoiceSetting(
                context,
                StringSetting.MAIN_GFX_BACKEND,
                R.string.video_backend,
                0,
                R.array.videoBackendEntries,
                R.array.videoBackendValues
            )
        )
        sl.add(
            SingleChoiceSettingDynamicDescriptions(
                context,
                IntSetting.GFX_SHADER_COMPILATION_MODE,
                R.string.shader_compilation_mode,
                0,
                R.array.shaderCompilationModeEntries,
                R.array.shaderCompilationModeValues,
                R.array.shaderCompilationDescriptionEntries,
                R.array.shaderCompilationDescriptionValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                R.string.wait_for_shaders,
                R.string.wait_for_shaders_description
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_ASPECT_RATIO,
                R.string.aspect_ratio,
                0,
                R.array.aspectRatioEntries,
                R.array.aspectRatioValues
            )
        )

        sl.add(HeaderSetting(context, R.string.graphics_more_settings, 0))
        sl.add(
            SubmenuSetting(
                context,
                R.string.enhancements_submenu,
                MenuTag.ENHANCEMENTS
            )
        )
        sl.add(
            SubmenuSetting(
                context,
                R.string.hacks_submenu,
                MenuTag.HACKS
            )
        )
        sl.add(
            SubmenuSetting(
                context,
                R.string.statistics_submenu,
                MenuTag.STATISTICS
            )
        )
        sl.add(
            SubmenuSetting(
                context,
                R.string.advanced_graphics_submenu,
                MenuTag.ADVANCED_GRAPHICS
            )
        )

        if (
            this.gpuDriver != null && this.gameId.isNullOrEmpty()
            && NativeLibrary.IsUninitialized()
            && GpuDriverHelper.supportsCustomDriverLoading()
        ) {
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.gpu_driver_submenu, MenuTag.GPU_DRIVERS
                )
            )
        }
    }

    private fun addEnhanceSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_EFB_SCALE,
                R.string.internal_resolution,
                R.string.internal_resolution_description,
                R.array.internalResolutionEntries,
                R.array.internalResolutionValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_MSAA,
                R.string.FSAA,
                R.string.FSAA_description,
                R.array.FSAAEntries,
                R.array.FSAAValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_ENHANCE_MAX_ANISOTROPY,
                R.string.anisotropic_filtering,
                R.string.anisotropic_filtering_description,
                R.array.anisotropicFilteringEntries,
                R.array.anisotropicFilteringValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                R.string.texture_filtering,
                R.string.texture_filtering_description,
                R.array.textureFilteringEntries,
                R.array.textureFilteringValues
            )
        )
        sl.add(
            SubmenuSetting(
                context,
                R.string.color_correction_submenu,
                MenuTag.COLOR_CORRECTION
            )
        )

        val stereoModeValue = IntSetting.GFX_STEREO_MODE.int
        val anaglyphMode = 3
        val shaderList =
            if (stereoModeValue == anaglyphMode) PostProcessing.anaglyphShaderList else PostProcessing.shaderList

        val shaderListEntries = arrayOf(context.getString(R.string.off), *shaderList)
        val shaderListValues = arrayOf("", *shaderList)

        sl.add(
            StringSingleChoiceSetting(
                context,
                StringSetting.GFX_ENHANCE_POST_SHADER,
                R.string.post_processing_shader,
                0,
                shaderListEntries,
                shaderListValues
            )
        )

        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_COPY_EFB_SCALED,
                R.string.scaled_efb_copy,
                R.string.scaled_efb_copy_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENABLE_PIXEL_LIGHTING,
                R.string.per_pixel_lighting,
                R.string.per_pixel_lighting_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENHANCE_FORCE_TRUE_COLOR,
                R.string.force_24bit_color,
                R.string.force_24bit_color_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DISABLE_FOG,
                R.string.disable_fog,
                R.string.disable_fog_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENHANCE_DISABLE_COPY_FILTER,
                R.string.disable_copy_filter,
                R.string.disable_copy_filter_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
                R.string.arbitrary_mipmap_detection,
                R.string.arbitrary_mipmap_detection_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_WIDESCREEN_HACK,
                R.string.wide_screen_hack,
                R.string.wide_screen_hack_description
            )
        )

        // Check if we support stereo
        // If we support desktop GL then we must support at least OpenGL 3.2
        // If we only support OpenGLES then we need both OpenGLES 3.1 and AEP
        val helper = EGLHelper(EGLHelper.EGL_OPENGL_ES2_BIT)

        if (BuildConfig.IS_QUEST ||
            helper.supportsOpenGL() && helper.GetVersion() >= 320 ||
            helper.supportsGLES3() && helper.GetVersion() >= 310 && helper.SupportsExtension(
                "GL_ANDROID_extension_pack_es31a"
            )
        ) {
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.stereoscopy_submenu,
                    MenuTag.STEREOSCOPY
                )
            )
        }
    }

    private fun addColorCorrectionSettings(sl: ArrayList<SettingsItem>) {
        sl.apply {
            add(HeaderSetting(context, R.string.color_space, 0))
            add(
                SwitchSetting(
                    context,
                    BooleanSetting.GFX_CC_CORRECT_COLOR_SPACE,
                    R.string.correct_color_space,
                    R.string.correct_color_space_description
                )
            )
            add(
                SingleChoiceSetting(
                    context,
                    IntSetting.GFX_CC_GAME_COLOR_SPACE,
                    R.string.game_color_space,
                    0,
                    R.array.colorSpaceEntries,
                    R.array.colorSpaceValues
                )
            )

            add(HeaderSetting(context, R.string.gamma, 0))
            add(
                FloatSliderSetting(
                    context,
                    FloatSetting.GFX_CC_GAME_GAMMA,
                    R.string.game_gamma,
                    R.string.game_gamma_description,
                    2.2f,
                    2.8f,
                    "",
                    0.01f,
                    true
                )
            )
            add(
                SwitchSetting(
                    context,
                    BooleanSetting.GFX_CC_CORRECT_GAMMA,
                    R.string.correct_sdr_gamma,
                    0
                )
            )
        }
    }

    private fun addHackSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.embedded_frame_buffer, 0))
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.GFX_HACK_EFB_ACCESS_ENABLE,
                R.string.skip_efb_access,
                R.string.skip_efb_access_description
            )
        )
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.GFX_HACK_EFB_EMULATE_FORMAT_CHANGES,
                R.string.ignore_format_changes,
                R.string.ignore_format_changes_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                R.string.efb_copy_method,
                R.string.efb_copy_method_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_DEFER_EFB_COPIES,
                R.string.defer_efb_copies,
                R.string.defer_efb_copies_description
            )
        )

        sl.add(HeaderSetting(context, R.string.texture_cache, 0))
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                R.string.texture_cache_accuracy,
                R.string.texture_cache_accuracy_description,
                R.array.textureCacheAccuracyEntries,
                R.array.textureCacheAccuracyValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENABLE_GPU_TEXTURE_DECODING,
                R.string.gpu_texture_decoding,
                R.string.gpu_texture_decoding_description
            )
        )

        sl.add(HeaderSetting(context, R.string.external_frame_buffer, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                R.string.xfb_copy_method,
                R.string.xfb_copy_method_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_IMMEDIATE_XFB,
                R.string.immediate_xfb,
                R.string.immediate_xfb_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_SKIP_DUPLICATE_XFBS,
                R.string.skip_duplicate_xfbs,
                R.string.skip_duplicate_xfbs_description
            )
        )

        sl.add(HeaderSetting(context, R.string.other, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_FAST_DEPTH_CALC,
                R.string.fast_depth_calculation,
                R.string.fast_depth_calculation_description
            )
        )
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.GFX_HACK_BBOX_ENABLE,
                R.string.disable_bbox,
                R.string.disable_bbox_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_VERTEX_ROUNDING,
                R.string.vertex_rounding,
                R.string.vertex_rounding_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_VI_SKIP,
                R.string.vi_skip,
                R.string.vi_skip_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SAVE_TEXTURE_CACHE_TO_STATE,
                R.string.texture_cache_to_state,
                R.string.texture_cache_to_state_description
            )
        )
    }

    private fun addStatisticsSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_FPS,
                R.string.show_fps,
                R.string.show_fps_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_FTIMES,
                R.string.show_ftimes,
                R.string.show_ftimes_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_VPS,
                R.string.show_vps,
                R.string.show_vps_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_VTIMES,
                R.string.show_vtimes,
                R.string.show_vtimes_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_GRAPHS,
                R.string.show_graphs,
                R.string.show_graphs_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_SPEED,
                R.string.show_speed,
                R.string.show_speed_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_SPEED_COLORS,
                R.string.show_speed_colors,
                R.string.show_speed_colors_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_LOG_RENDER_TIME_TO_FILE,
                R.string.log_render_time_to_file,
                R.string.log_render_time_to_file_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.GFX_PERF_SAMP_WINDOW,
                R.string.performance_sample_window,
                R.string.performance_sample_window_description,
                0,
                10000,
                "ms",
                100
            )
        )
    }

    private fun addAdvancedGraphicsSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.gfx_mods_and_custom_textures, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_MODS_ENABLE,
                R.string.gfx_mods,
                R.string.gfx_mods_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HIRES_TEXTURES,
                R.string.load_custom_texture,
                R.string.load_custom_texture_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_CACHE_HIRES_TEXTURES,
                R.string.cache_custom_texture,
                R.string.cache_custom_texture_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DUMP_TEXTURES,
                R.string.dump_texture,
                R.string.dump_texture_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DUMP_BASE_TEXTURES,
                R.string.dump_base_texture,
                R.string.dump_base_texture_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DUMP_MIP_TEXTURES,
                R.string.dump_mip_texture,
                R.string.dump_mip_texture_description
            )
        )

        sl.add(HeaderSetting(context, R.string.misc, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_CROP,
                R.string.crop,
                R.string.crop_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.SYSCONF_PROGRESSIVE_SCAN,
                R.string.progressive_scan,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_VSYNC,
                R.string.vsync,
                R.string.vsync_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_BACKEND_MULTITHREADING,
                R.string.backend_multithreading,
                R.string.backend_multithreading_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.GFX_COMMAND_BUFFERS_IN_FLIGHT,
                R.string.command_buffers_in_flight,
                R.string.command_buffers_in_flight_description,
                2,
                32,
                "",
                1
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION,
                R.string.prefer_vs_for_point_line_expansion,
                R.string.prefer_vs_for_point_line_expansion_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_CPU_CULL,
                R.string.cpu_cull,
                R.string.cpu_cull_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_EFB_DEFER_INVALIDATION,
                R.string.defer_efb_invalidation,
                R.string.defer_efb_invalidation_description
            )
        )
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.GFX_HACK_FAST_TEXTURE_SAMPLING,
                R.string.manual_texture_sampling,
                R.string.manual_texture_sampling_description
            )
        )

        sl.add(HeaderSetting(context, R.string.frame_dumping, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_INTERNAL_RESOLUTION_FRAME_DUMPS,
                R.string.internal_resolution_dumps,
                R.string.internal_resolution_dumps_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                IntSetting.GFX_PNG_COMPRESSION_LEVEL,
                R.string.png_compression_level,
                0,
                0,
                9,
                "",
                1
            )
        )

        sl.add(HeaderSetting(context, R.string.debugging, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENABLE_WIREFRAME,
                R.string.wireframe,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_OVERLAY_STATS,
                R.string.show_stats,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_TEXFMT_OVERLAY_ENABLE,
                R.string.texture_format,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_ENABLE_VALIDATION_LAYER,
                R.string.validation_layer,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DUMP_EFB_TARGET,
                R.string.dump_efb,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_DUMP_XFB_TARGET,
                R.string.dump_xfb,
                R.string.leave_this_unchecked
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_HACK_DISABLE_COPY_TO_VRAM,
                R.string.disable_vram_copies,
                R.string.leave_this_unchecked
            )
        )
    }

    private fun addLogConfigurationSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.LOGGER_WRITE_TO_FILE,
                R.string.log_to_file,
                R.string.log_to_file_description
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.LOGGER_VERBOSITY,
                R.string.log_verbosity,
                0,
                getLogVerbosityEntries(), getLogVerbosityValues()
            )
        )
        sl.add(
            RunRunnable(
                context,
                R.string.log_enable_all,
                0,
                R.string.log_enable_all_confirmation,
                0,
                true
            ) { setAllLogTypes(true) })
        sl.add(
            RunRunnable(
                context,
                R.string.log_disable_all,
                0,
                R.string.log_disable_all_confirmation,
                0,
                true
            ) { setAllLogTypes(false) })
        sl.add(
            RunRunnable(
                context,
                R.string.log_clear,
                0,
                R.string.log_clear_confirmation,
                0,
                true
            ) { SettingsAdapter.clearLog() })

        sl.add(HeaderSetting(context, R.string.log_types, 0))
        for (logType in NativeLibrary.GetLogTypeNames()) {
            sl.add(LogSwitchSetting(logType.first, logType.second, ""))
        }
    }

    private fun addDebugSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.debug_warning, 0))
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.MAIN_FASTMEM,
                R.string.debug_fastmem,
                0
            )
        )
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.MAIN_FASTMEM_ARENA,
                R.string.debug_fastmem_arena,
                0
            )
        )
        sl.add(
            InvertedSwitchSetting(
                context,
                BooleanSetting.MAIN_LARGE_ENTRY_POINTS_MAP,
                R.string.debug_large_entry_points_map,
                0
            )
        )

        sl.add(HeaderSetting(context, R.string.debug_jit_profiling_header, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_ENABLE_PROFILING,
                R.string.debug_jit_enable_block_profiling,
                0
           )
        )
        sl.add(
            RunRunnable(
                context,
                R.string.debug_jit_wipe_block_profiling_data,
                0,
                R.string.debug_jit_wipe_block_profiling_data_alert,
                0,
                true
            ) { NativeLibrary.WipeJitBlockProfilingData() }
        )
        sl.add(
            RunRunnable(
                context,
                R.string.debug_jit_write_block_log_dump,
                0,
                0,
                0,
                true
            ) { NativeLibrary.WriteJitBlockLogDump() }
        )

        sl.add(HeaderSetting(context, R.string.debug_jit_header, 0))
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_OFF,
                R.string.debug_jitoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_LOAD_STORE_OFF,
                R.string.debug_jitloadstoreoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF,
                R.string.debug_jitloadstorefloatingoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF,
                R.string.debug_jitloadstorepairedoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_FLOATING_POINT_OFF,
                R.string.debug_jitfloatingpointoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_INTEGER_OFF,
                R.string.debug_jitintegeroff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_PAIRED_OFF,
                R.string.debug_jitpairedoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF,
                R.string.debug_jitsystemregistersoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_BRANCH_OFF,
                R.string.debug_jitbranchoff,
                0
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.MAIN_DEBUG_JIT_REGISTER_CACHE_OFF,
                R.string.debug_jitregistercacheoff,
                0
            )
        )
    }

    private fun addStereoSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SingleChoiceSetting(
                context,
                IntSetting.GFX_STEREO_MODE,
                R.string.stereoscopy_mode,
                0,
                R.array.stereoscopyEntries,
                R.array.stereoscopyValues
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                FloatSetting.GFX_STEREO_DEPTH,
                R.string.stereoscopy_depth,
                R.string.stereoscopy_depth_description,
                0f,
                100f,
                "",
                1f,
                false
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                FloatSetting.GFX_STEREO_CONVERGENCE,
                R.string.stereoscopy_convergence,
                R.string.stereoscopy_convergence_description,
                0f,
                200f,
                "",
                0.01f,
                true
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_STEREO_SWAP_EYES,
                R.string.stereoscopy_swap_eyes,
                R.string.stereoscopy_swap_eyes_description
            )
        )
    }

    private fun addOpenXrSettings(sl: ArrayList<SettingsItem>) {
        addQuestVrSettings(sl)
    }

    private fun addQuestOverrideSettings(
        sl: ArrayList<SettingsItem>,
        category: QuestGameOverrideSettings.Category
    ) {
        val currentGameId = gameId
        if (currentGameId.isNullOrEmpty()) {
            sl.add(HeaderSetting(context, R.string.quest_no_stored_overrides, 0))
            return
        }

        val entries = QuestGameOverrideSettings.loadEntries(currentGameId, revision, category)
        if (entries.isEmpty()) {
            sl.add(HeaderSetting(context, R.string.quest_no_stored_overrides, 0))
            return
        }

        val descriptionId = when (category) {
            QuestGameOverrideSettings.Category.HIDE_OBJECTS ->
                R.string.quest_hide_objects_description
            QuestGameOverrideSettings.Category.SHADER_OVERRIDES ->
                R.string.quest_shader_overrides_description
            QuestGameOverrideSettings.Category.ELEMENTS_GROUP_OVERRIDES ->
                R.string.quest_elements_group_overrides_description
            QuestGameOverrideSettings.Category.TEXTURE_ELEMENT_OVERRIDES ->
                R.string.quest_texture_element_overrides_description
        }

        for (entry in entries) {
            sl.add(
                SwitchSetting(
                    QuestGameOverrideEnabledSetting(currentGameId, revision, category, entry.name),
                    entry.name,
                    context.getString(descriptionId)
                )
            )
        }
    }

    /**
     * Builds the per-game VR Config rows for one group. Holds the game identity so each group
     * function reads like the global OpenXR screen instead of repeating the setting plumbing.
     */
    private inner class QuestGameVrConfigBuilder(
        private val sl: ArrayList<SettingsItem>,
        private val gameId: String
    ) {
        fun boolean(key: String, defaultValue: Boolean, titleId: Int, descriptionId: Int) {
            sl.add(
                SwitchSetting(
                    context,
                    QuestGameVrConfigBooleanSetting(gameId, revision, key, defaultValue),
                    titleId,
                    descriptionId
                )
            )
        }

        fun float(
            key: String,
            defaultValue: Float,
            titleId: Int,
            descriptionId: Int,
            min: Float,
            max: Float,
            step: Float
        ) {
            sl.add(
                FloatSliderSetting(
                    context,
                    QuestGameVrConfigFloatSetting(gameId, revision, key, defaultValue),
                    titleId,
                    descriptionId,
                    min,
                    max,
                    "",
                    step,
                    true
                )
            )
        }

        fun intSlider(
            key: String,
            defaultValue: Int,
            titleId: Int,
            descriptionId: Int,
            min: Int,
            max: Int,
            step: Int
        ) {
            sl.add(
                IntSliderSetting(
                    context,
                    QuestGameVrConfigIntSetting(gameId, revision, key, defaultValue),
                    titleId,
                    descriptionId,
                    min,
                    max,
                    "",
                    step
                )
            )
        }

        fun choice(
            key: String,
            defaultValue: Int,
            titleId: Int,
            descriptionId: Int,
            entriesId: Int,
            valuesId: Int
        ) {
            sl.add(
                SingleChoiceSetting(
                    context,
                    QuestGameVrConfigIntSetting(gameId, revision, key, defaultValue),
                    titleId,
                    descriptionId,
                    entriesId,
                    valuesId
                )
            )
        }
    }

    private fun questGameVrConfigBuilder(sl: ArrayList<SettingsItem>): QuestGameVrConfigBuilder? {
        val currentGameId = gameId
        if (currentGameId.isNullOrEmpty()) {
            sl.add(HeaderSetting(context, R.string.quest_no_stored_vr_config, 0))
            return null
        }
        return QuestGameVrConfigBuilder(sl, currentGameId)
    }

    private fun addQuestGameVrConfigSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        sl.add(HeaderSetting(context, R.string.quest_vr_config_description, 0))
        builder.boolean(
            "EnableOpenXR",
            false,
            R.string.quest_enable_openxr,
            R.string.quest_enable_openxr_description
        )
        builder.float(
            "UnitsPerMeter",
            1.0f,
            R.string.quest_units_per_meter,
            R.string.quest_units_per_meter_description,
            0.1f,
            500.0f,
            0.1f
        )

        sl.add(HeaderSetting(context, R.string.quest_vr_groups, 0))
        sl.add(SubmenuSetting(context, R.string.quest_vr_camera, MenuTag.QUEST_VR_CONFIG_CAMERA))
        sl.add(
            SubmenuSetting(
                context,
                R.string.quest_vr_virtual_screen,
                MenuTag.QUEST_VR_CONFIG_VIRTUAL_SCREEN
            )
        )
        sl.add(
            SubmenuSetting(context, R.string.quest_vr_rendering, MenuTag.QUEST_VR_CONFIG_RENDERING)
        )
        sl.add(
            SubmenuSetting(context, R.string.quest_vr_framerate, MenuTag.QUEST_VR_CONFIG_FRAMERATE)
        )
        sl.add(SubmenuSetting(context, R.string.quest_vr_hacks, MenuTag.QUEST_VR_CONFIG_HACKS))
        sl.add(
            SubmenuSetting(
                context,
                R.string.quest_vr_passthrough,
                MenuTag.QUEST_VR_CONFIG_PASSTHROUGH
            )
        )
        sl.add(SubmenuSetting(context, R.string.quest_vr_debug, MenuTag.QUEST_VR_CONFIG_DEBUG))
    }

    private fun addQuestGameVrCameraSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.boolean(
            "EnableLeanBackAngle",
            true,
            R.string.quest_enable_lean_back_angle,
            R.string.quest_enable_lean_back_angle_description
        )
        builder.float(
            "LeanBackAngle",
            0.0f,
            R.string.quest_lean_back_angle,
            R.string.quest_lean_back_angle_description,
            -45.0f,
            45.0f,
            0.1f
        )
        builder.boolean(
            "EnableCameraForward",
            true,
            R.string.quest_enable_camera_forward,
            R.string.quest_enable_camera_forward_description
        )
        builder.float(
            "CameraForward",
            0.0f,
            R.string.quest_camera_forward,
            R.string.quest_camera_forward_description,
            -20.0f,
            20.0f,
            0.1f
        )
        builder.boolean(
            "EnableCameraHeight",
            true,
            R.string.quest_enable_camera_height,
            R.string.quest_enable_camera_height_description
        )
        builder.float(
            "CameraHeight",
            0.0f,
            R.string.quest_camera_height,
            R.string.quest_camera_height_description,
            -20.0f,
            20.0f,
            0.1f
        )
        builder.boolean(
            "EnableCameraAnchor",
            false,
            R.string.quest_enable_camera_anchor,
            R.string.quest_enable_camera_anchor_description
        )
        builder.float(
            "CameraAnchorSmoothing",
            0.85f,
            R.string.quest_camera_anchor_smoothing,
            R.string.quest_camera_anchor_smoothing_description,
            0.0f,
            0.95f,
            0.05f
        )
        builder.boolean(
            "EnableControllerAnchor",
            false,
            R.string.quest_enable_controller_anchor,
            R.string.quest_enable_controller_anchor_description
        )
    }

    private fun addQuestGameVrVirtualScreenSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.boolean(
            "VirtualScreen",
            true,
            R.string.quest_virtual_screen,
            R.string.quest_virtual_screen_description
        )
        builder.boolean(
            "ExactScreenDepth",
            true,
            R.string.quest_exact_screen_depth,
            R.string.quest_exact_screen_depth_description
        )
        builder.boolean(
            "AutoNativeEfbEffects",
            true,
            R.string.quest_auto_native_efb_effects,
            R.string.quest_auto_native_efb_effects_description
        )
        builder.float(
            "ScreenDistance",
            1.5f,
            R.string.quest_screen_distance,
            R.string.quest_screen_distance_description,
            0.5f,
            10.0f,
            0.1f
        )
        builder.float(
            "ScreenSize",
            1.5f,
            R.string.quest_screen_size,
            R.string.quest_screen_size_description,
            0.5f,
            5.0f,
            0.1f
        )
        builder.float(
            "HudThickness",
            0.0f,
            R.string.quest_hud_thickness,
            R.string.quest_hud_thickness_description,
            0.0f,
            1.0f,
            0.02f
        )
        builder.float(
            "HeadLockedCurvature",
            0.0f,
            R.string.quest_head_locked_curvature,
            R.string.quest_head_locked_curvature_description,
            0.0f,
            5.0f,
            0.01f
        )
    }

    private fun addQuestGameVrRenderingSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.float(
            "ResolutionScale",
            0.85f,
            R.string.quest_resolution_scale,
            R.string.quest_resolution_scale_description,
            0.5f,
            1.5f,
            0.05f
        )
        builder.choice(
            "FoveationLevel",
            2,
            R.string.quest_foveation_level,
            R.string.quest_foveation_level_description,
            R.array.questFoveationLevelEntries,
            R.array.questFoveationLevelValues
        )
        builder.boolean(
            "DynamicFoveation",
            true,
            R.string.quest_dynamic_foveation,
            R.string.quest_dynamic_foveation_description
        )
        builder.boolean(
            "FoveateEFB",
            false,
            R.string.quest_foveate_efb,
            R.string.quest_foveate_efb_description
        )
        builder.intSlider(
            "ClearEFBCopies",
            0,
            R.string.quest_clear_efb_copies,
            R.string.quest_clear_efb_copies_description,
            0,
            640,
            10
        )
        builder.float(
            "Gamma",
            1.0f,
            R.string.quest_vr_gamma,
            R.string.quest_vr_gamma_description,
            1.0f,
            3.0f,
            0.1f
        )
    }

    private fun addQuestGameVrFramerateSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.choice(
            "ForcedVBIFrequency",
            0,
            R.string.quest_forced_vbi_frequency,
            R.string.quest_forced_vbi_frequency_description,
            R.array.questForcedVbiFrequencyEntries,
            R.array.questForcedVbiFrequencyValues
        )
        builder.boolean(
            "EagerHeartbeat",
            false,
            R.string.quest_eager_heartbeat,
            R.string.quest_eager_heartbeat_description
        )
    }

    private fun addQuestGameVrHackSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.boolean(
            "UseVulkanMultiview",
            true,
            R.string.quest_use_vulkan_multiview,
            R.string.quest_use_vulkan_multiview_description
        )
        builder.boolean(
            "DontClearScreen",
            false,
            R.string.quest_dont_clear_screen,
            R.string.quest_dont_clear_screen_description
        )
        builder.boolean(
            "DisableCPUCull",
            false,
            R.string.quest_disable_cpu_culling,
            R.string.quest_disable_cpu_culling_description
        )
        builder.boolean(
            "RemoveCinematicBars",
            true,
            R.string.quest_remove_cinematic_bars,
            R.string.quest_remove_cinematic_bars_description
        )
        builder.boolean(
            "FrameSizeFromXFB",
            true,
            R.string.quest_frame_size_from_xfb,
            R.string.quest_frame_size_from_xfb_description
        )
        builder.boolean(
            "SmallViewportsOnScreen",
            true,
            R.string.quest_small_viewports_on_screen,
            R.string.quest_small_viewports_on_screen_description
        )
        builder.boolean(
            "DetectRenderTargets",
            false,
            R.string.quest_detect_render_targets,
            R.string.quest_detect_render_targets_description
        )
        builder.boolean(
            "OrthoScissorFix",
            true,
            R.string.quest_ortho_scissor_fix,
            R.string.quest_ortho_scissor_fix_description
        )
        builder.boolean(
            "DetectSkybox",
            false,
            R.string.quest_detect_skybox,
            R.string.quest_detect_skybox_description
        )
        builder.boolean(
            "MetroidThermalVisorFix",
            true,
            R.string.quest_layered_palette_conversion,
            R.string.quest_layered_palette_conversion_description
        )
    }

    private fun addQuestGameVrPassthroughSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.boolean(
            "Passthrough",
            false,
            R.string.quest_passthrough,
            R.string.quest_passthrough_description
        )
        builder.boolean(
            "PassthroughRemoveBlackBackground",
            true,
            R.string.quest_passthrough_reveal_unrendered,
            R.string.quest_passthrough_reveal_unrendered_description
        )
        builder.boolean(
            "PassthroughRemoveBlackEFBClears",
            true,
            R.string.quest_passthrough_remove_black_clears,
            R.string.quest_passthrough_remove_black_clears_description
        )
        builder.float(
            "PassthroughSceneOpacity",
            1.0f,
            R.string.quest_passthrough_scene_opacity,
            R.string.quest_passthrough_scene_opacity_description,
            0.0f,
            1.0f,
            0.05f
        )
        builder.choice(
            "PassthroughCoverageMode",
            0,
            R.string.quest_passthrough_coverage_mode,
            R.string.quest_passthrough_coverage_mode_description,
            R.array.questPassthroughCoverageModeEntries,
            R.array.questPassthroughCoverageModeValues
        )
    }

    private fun addQuestGameVrDebugSettings(sl: ArrayList<SettingsItem>) {
        val builder = questGameVrConfigBuilder(sl) ?: return

        builder.boolean(
            "AndroidDirectToHMD",
            false,
            R.string.quest_android_direct_to_hmd,
            R.string.quest_android_direct_to_hmd_description
        )
        builder.boolean(
            "LoadCustomShaders",
            false,
            R.string.quest_load_custom_shaders,
            R.string.quest_load_custom_shaders_description
        )
    }

    private fun addQuestVrSettings(sl: ArrayList<SettingsItem>) {
        sl.add(HeaderSetting(context, R.string.quest_vr_runtime, 0))
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.openXrEnabledSetting(),
                R.string.quest_enable_openxr,
                R.string.quest_enable_openxr_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.launchInVrSetting(),
                R.string.quest_launch_in_vr,
                R.string.quest_launch_in_vr_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.recenterOnLaunchSetting(),
                R.string.quest_recenter_on_launch,
                R.string.quest_recenter_on_launch_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.unitsPerMeterSetting(),
                R.string.quest_units_per_meter,
                R.string.quest_units_per_meter_description,
                0.1f,
                500.0f,
                "",
                0.1f,
                true
            )
        )

        sl.add(HeaderSetting(context, R.string.quest_vr_groups, 0))
        sl.add(SubmenuSetting(context, R.string.quest_vr_camera, MenuTag.QUEST_VR_CAMERA))
        sl.add(
            SubmenuSetting(
                context,
                R.string.quest_vr_virtual_screen,
                MenuTag.QUEST_VR_VIRTUAL_SCREEN
            )
        )
        sl.add(SubmenuSetting(context, R.string.quest_vr_rendering, MenuTag.QUEST_VR_RENDERING))
        sl.add(SubmenuSetting(context, R.string.quest_vr_framerate, MenuTag.QUEST_VR_FRAMERATE))
        sl.add(SubmenuSetting(context, R.string.quest_vr_hacks, MenuTag.QUEST_VR_HACKS))
        sl.add(SubmenuSetting(context, R.string.quest_vr_passthrough, MenuTag.QUEST_VR_PASSTHROUGH))
        sl.add(SubmenuSetting(context, R.string.quest_vr_debug, MenuTag.QUEST_VR_DEBUG))
    }

    private fun addQuestVrCameraSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.enableLeanBackAngleSetting(),
                R.string.quest_enable_lean_back_angle,
                R.string.quest_enable_lean_back_angle_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.leanBackAngleSetting(),
                R.string.quest_lean_back_angle,
                R.string.quest_lean_back_angle_description,
                -45.0f,
                45.0f,
                "",
                0.1f,
                true
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.enableCameraForwardSetting(),
                R.string.quest_enable_camera_forward,
                R.string.quest_enable_camera_forward_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.cameraForwardSetting(),
                R.string.quest_camera_forward,
                R.string.quest_camera_forward_description,
                -20.0f,
                20.0f,
                "",
                0.1f,
                true
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.enableCameraHeightSetting(),
                R.string.quest_enable_camera_height,
                R.string.quest_enable_camera_height_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.cameraHeightSetting(),
                R.string.quest_camera_height,
                R.string.quest_camera_height_description,
                -20.0f,
                20.0f,
                "",
                0.1f,
                true
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.enableCameraAnchorSetting(),
                R.string.quest_enable_camera_anchor,
                R.string.quest_enable_camera_anchor_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.cameraAnchorSmoothingSetting(),
                R.string.quest_camera_anchor_smoothing,
                R.string.quest_camera_anchor_smoothing_description,
                0.0f,
                0.95f,
                "",
                0.05f,
                true
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.enableControllerAnchorSetting(),
                R.string.quest_enable_controller_anchor,
                R.string.quest_enable_controller_anchor_description
            )
        )
    }

    private fun addQuestVrVirtualScreenSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.virtualScreenSetting(),
                R.string.quest_virtual_screen,
                R.string.quest_virtual_screen_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.exactScreenDepthSetting(),
                R.string.quest_exact_screen_depth,
                R.string.quest_exact_screen_depth_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.autoNativeEfbEffectsSetting(),
                R.string.quest_auto_native_efb_effects,
                R.string.quest_auto_native_efb_effects_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.screenDistanceSetting(),
                R.string.quest_screen_distance,
                R.string.quest_screen_distance_description,
                0.5f,
                10.0f,
                "",
                0.1f,
                true
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.screenSizeSetting(),
                R.string.quest_screen_size,
                R.string.quest_screen_size_description,
                0.5f,
                5.0f,
                "",
                0.1f,
                true
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.hudThicknessSetting(),
                R.string.quest_hud_thickness,
                R.string.quest_hud_thickness_description,
                0.0f,
                1.0f,
                "",
                0.02f,
                true
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.headLockedCurvatureSetting(),
                R.string.quest_head_locked_curvature,
                R.string.quest_head_locked_curvature_description,
                0.0f,
                5.0f,
                "",
                0.01f,
                true
            )
        )
    }

    private fun addQuestVrRenderingSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.resolutionScaleSetting(),
                R.string.quest_resolution_scale,
                R.string.quest_resolution_scale_description,
                0.5f,
                1.5f,
                "",
                0.05f,
                true
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                QuestVrSettings.foveationLevelSetting(),
                R.string.quest_foveation_level,
                R.string.quest_foveation_level_description,
                R.array.questFoveationLevelEntries,
                R.array.questFoveationLevelValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.dynamicFoveationSetting(),
                R.string.quest_dynamic_foveation,
                R.string.quest_dynamic_foveation_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.foveateEfbSetting(),
                R.string.quest_foveate_efb,
                R.string.quest_foveate_efb_description
            )
        )
        sl.add(
            IntSliderSetting(
                context,
                QuestVrSettings.clearEfbCopiesSetting(),
                R.string.quest_clear_efb_copies,
                R.string.quest_clear_efb_copies_description,
                0,
                640,
                "",
                10
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.vrGammaSetting(),
                R.string.quest_vr_gamma,
                R.string.quest_vr_gamma_description,
                1.0f,
                3.0f,
                "",
                0.1f,
                true
            )
        )
    }

    private fun addQuestVrFramerateSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SingleChoiceSetting(
                context,
                QuestVrSettings.forcedVbiFrequencySetting(),
                R.string.quest_forced_vbi_frequency,
                R.string.quest_forced_vbi_frequency_description,
                R.array.questForcedVbiFrequencyEntries,
                R.array.questForcedVbiFrequencyValues
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.eagerHeartbeatSetting(),
                R.string.quest_eager_heartbeat,
                R.string.quest_eager_heartbeat_description
            )
        )
    }

    private fun addQuestVrHackSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.useVulkanMultiviewSetting(),
                R.string.quest_use_vulkan_multiview,
                R.string.quest_use_vulkan_multiview_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.dontClearScreenSetting(),
                R.string.quest_dont_clear_screen,
                R.string.quest_dont_clear_screen_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.disableCpuCullSetting(),
                R.string.quest_disable_cpu_culling,
                R.string.quest_disable_cpu_culling_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.removeBarsSetting(),
                R.string.quest_remove_cinematic_bars,
                R.string.quest_remove_cinematic_bars_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.frameSizeFromXfbSetting(),
                R.string.quest_frame_size_from_xfb,
                R.string.quest_frame_size_from_xfb_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.panesOnScreenSetting(),
                R.string.quest_small_viewports_on_screen,
                R.string.quest_small_viewports_on_screen_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.detectRenderTargetsSetting(),
                R.string.quest_detect_render_targets,
                R.string.quest_detect_render_targets_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.orthoScissorFixSetting(),
                R.string.quest_ortho_scissor_fix,
                R.string.quest_ortho_scissor_fix_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.detectSkyboxSetting(),
                R.string.quest_detect_skybox,
                R.string.quest_detect_skybox_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.layeredPaletteConversionSetting(),
                R.string.quest_layered_palette_conversion,
                R.string.quest_layered_palette_conversion_description
            )
        )
    }

    private fun addQuestVrPassthroughSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.passthroughSetting(),
                R.string.quest_passthrough,
                R.string.quest_passthrough_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.passthroughRevealUnrenderedSetting(),
                R.string.quest_passthrough_reveal_unrendered,
                R.string.quest_passthrough_reveal_unrendered_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.passthroughRemoveBlackClearsSetting(),
                R.string.quest_passthrough_remove_black_clears,
                R.string.quest_passthrough_remove_black_clears_description
            )
        )
        sl.add(
            FloatSliderSetting(
                context,
                QuestVrSettings.passthroughSceneOpacitySetting(),
                R.string.quest_passthrough_scene_opacity,
                R.string.quest_passthrough_scene_opacity_description,
                0.0f,
                1.0f,
                "",
                0.05f,
                true
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                QuestVrSettings.passthroughCoverageModeSetting(),
                R.string.quest_passthrough_coverage_mode,
                R.string.quest_passthrough_coverage_mode_description,
                R.array.questPassthroughCoverageModeEntries,
                R.array.questPassthroughCoverageModeValues
            )
        )
    }

    private fun addQuestVrDebugSettings(sl: ArrayList<SettingsItem>) {
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.leftHandedSetting(),
                R.string.quest_left_handed_wiimote,
                R.string.quest_left_handed_wiimote_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.showMirrorSurfaceSetting(),
                R.string.quest_show_mirror_surface,
                R.string.quest_show_mirror_surface_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                BooleanSetting.GFX_SHOW_FPS,
                R.string.quest_show_perf_hud,
                R.string.quest_show_perf_hud_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.androidDirectToHmdSetting(),
                R.string.quest_android_direct_to_hmd,
                R.string.quest_android_direct_to_hmd_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.cpuLevel5HintSetting(),
                R.string.quest_cpu_level_5_hint,
                R.string.quest_cpu_level_5_hint_description
            )
        )
        sl.add(
            SwitchSetting(
                context,
                QuestVrSettings.loadCustomShadersSetting(),
                R.string.quest_load_custom_shaders,
                R.string.quest_load_custom_shaders_description
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                QuestVrSettings.referenceSpaceModeSetting(),
                R.string.quest_reference_space_mode,
                R.string.quest_reference_space_mode_description,
                R.array.questReferenceSpaceModeEntries,
                R.array.questReferenceSpaceModeValues
            )
        )
        sl.add(
            SingleChoiceSetting(
                context,
                QuestVrSettings.trackingModeSetting(),
                R.string.quest_tracking_mode,
                R.string.quest_tracking_mode_description,
                R.array.questTrackingModeEntries,
                R.array.questTrackingModeValues
            )
        )
        sl.add(
            RunRunnable(
                context,
                R.string.quest_recenter_now,
                R.string.quest_recenter_now_description,
                0,
                0,
                true
            ) { NativeLibrary.RequestOpenXRRecenter() }
        )
        sl.add(
            RunRunnable(
                context,
                R.string.quest_reset_openxr_settings,
                R.string.quest_reset_openxr_settings_description,
                R.string.quest_reset_openxr_settings_confirmation,
                R.string.quest_openxr_settings_reset,
                false
            ) {
                runQuestSettingsMutation { settings ->
                    QuestVrSettings.resetOpenXrSettings(settings)
                }
            }
        )
    }

    private fun runQuestSettingsMutation(block: (Settings) -> Unit) {
        val activeSettings = settings ?: return
        block(activeSettings)
        activeSettings.saveSettings()
        fragmentView.adapter?.notifyAllSettingsChanged()
    }

    private fun addOpenXRControllerMapperSetting(
        sl: ArrayList<SettingsItem>,
        controllerPort: Int,
        targetType: Int
    ) {
        sl.add(
            RunRunnable(
                context,
                R.string.openxr_controller_mapper,
                R.string.openxr_controller_mapper_description,
                0,
                0,
                true
            ) {
                context.startActivity(
                    Intent(context, OpenXRControllerMapperActivity::class.java)
                        .setAction(Intent.ACTION_MAIN)
                        .addCategory("com.oculus.intent.category.VR")
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
                        .putExtra(
                            OpenXRControllerMapperActivity.EXTRA_CONTROLLER_PORT,
                            controllerPort
                        )
                        .putExtra(OpenXRControllerMapperActivity.EXTRA_TARGET_TYPE, targetType)
                )
            }
        )
    }

    private fun addGcPadSubSettings(sl: ArrayList<SettingsItem>, gcPadNumber: Int, gcPadType: Int) {
        when (gcPadType) {
            6, 8, 9, 10, 11 -> {
                // Emulated
                val gcPad = EmulatedController.getGcPad(gcPadNumber)

                if (!TextUtils.isEmpty(gameId)) {
                    addControllerPerGameSettings(sl, gcPad, gcPadNumber)
                } else {
                    addControllerMetaSettings(sl, gcPad, QuestVrSettings.GC_PROFILE_NAME)
                    if (BuildConfig.IS_QUEST && gcPad.getDefaultDevice() == OPENXR_CONTROLLER_DEVICE) {
                        addOpenXRControllerMapperSetting(
                            sl,
                            gcPadNumber,
                            OpenXRControllerMapperActivity.TARGET_GAMECUBE_CONTROLLER
                        )
                    }
                    addControllerMappingSettings(sl, gcPad, null)
                }
            }
            7 -> {
                // Emulated keyboard controller
                val gcKeyboard = EmulatedController.getGcKeyboard(gcPadNumber)

                if (!TextUtils.isEmpty(gameId)) {
                    addControllerPerGameSettings(sl, gcKeyboard, gcPadNumber)
                } else {
                    sl.add(HeaderSetting(context, R.string.keyboard_controller_warning, 0))
                    addControllerMetaSettings(sl, gcKeyboard)
                    addControllerMappingSettings(sl, gcKeyboard, null)
                }
            }
            12 -> {
                // Adapter
                sl.add(
                    SwitchSetting(
                        context,
                        BooleanSetting.getSettingForAdapterRumble(gcPadNumber),
                        R.string.gc_adapter_rumble,
                        R.string.gc_adapter_rumble_description
                    )
                )
                sl.add(
                    SwitchSetting(
                        context,
                        BooleanSetting.getSettingForSimulateKonga(gcPadNumber),
                        R.string.gc_adapter_bongos,
                        R.string.gc_adapter_bongos_description
                    )
                )
            }
        }
    }

    private fun addWiimoteSubSettings(sl: ArrayList<SettingsItem>, wiimoteNumber: Int) {
        val wiimote = EmulatedController.getWiimote(wiimoteNumber)

        if (!TextUtils.isEmpty(gameId)) {
            addControllerPerGameSettings(sl, wiimote, wiimoteNumber)
        } else {
            addControllerMetaSettings(sl, wiimote, QuestVrSettings.WIIMOTE_PROFILE_NAME)

            val sourceSetting = when (wiimoteNumber) {
                0 -> IntSetting.WIIMOTE_1_SOURCE
                1 -> IntSetting.WIIMOTE_2_SOURCE
                2 -> IntSetting.WIIMOTE_3_SOURCE
                else -> IntSetting.WIIMOTE_4_SOURCE
            }
            if (BuildConfig.IS_QUEST && sourceSetting.int == 3)
                addOpenXRControllerMapperSetting(
                    sl,
                    wiimoteNumber,
                    OpenXRControllerMapperActivity.TARGET_WII_REMOTE
                )

            sl.add(HeaderSetting(context, R.string.wiimote, 0))
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.wiimote_general,
                    MenuTag.getWiimoteGeneralMenuTag(wiimoteNumber)
                )
            )
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.wiimote_motion_simulation,
                    MenuTag.getWiimoteMotionSimulationMenuTag(wiimoteNumber)
                )
            )
            sl.add(
                SubmenuSetting(
                    context,
                    R.string.wiimote_motion_input,
                    MenuTag.getWiimoteMotionInputMenuTag(wiimoteNumber)
                )
            )

            // TYPE_OTHER is included here instead of in addWiimoteGeneralSubSettings so that touchscreen
            // users won't have to dig into a submenu to find the Sideways Wii Remote setting
            addControllerMappingSettings(
                sl,
                wiimote,
                ArraySet(listOf(ControlGroup.TYPE_ATTACHMENTS, ControlGroup.TYPE_OTHER))
            )
        }
    }

    private fun addExtensionTypeSettings(
        sl: ArrayList<SettingsItem>,
        wiimoteNumber: Int,
        extensionType: Int
    ) {
        addContainerMappingSettings(
            sl,
            EmulatedController.getWiimote(wiimoteNumber),
            EmulatedController.getWiimoteAttachment(wiimoteNumber, extensionType),
            null
        )
    }

    private fun addWiimoteGeneralSubSettings(sl: ArrayList<SettingsItem>, wiimoteNumber: Int) {
        addControllerMappingSettings(
            sl,
            EmulatedController.getWiimote(wiimoteNumber),
            setOf(ControlGroup.TYPE_BUTTONS)
        )
    }

    private fun addWiimoteMotionSimulationSubSettings(
        sl: ArrayList<SettingsItem>,
        wiimoteNumber: Int
    ) {
        addControllerMappingSettings(
            sl, EmulatedController.getWiimote(wiimoteNumber),
            ArraySet(
                listOf(
                    ControlGroup.TYPE_FORCE,
                    ControlGroup.TYPE_TILT,
                    ControlGroup.TYPE_CURSOR,
                    ControlGroup.TYPE_SHAKE
                )
            )
        )
    }

    private fun addWiimoteMotionInputSubSettings(sl: ArrayList<SettingsItem>, wiimoteNumber: Int) {
        addControllerMappingSettings(
            sl, EmulatedController.getWiimote(wiimoteNumber),
            ArraySet(
                listOf(
                    ControlGroup.TYPE_IMU_ACCELEROMETER,
                    ControlGroup.TYPE_IMU_GYROSCOPE,
                    ControlGroup.TYPE_IMU_CURSOR
                )
            )
        )
    }

    // Only groups that have at least one action wired in HotkeyDispatcher.cpp are listed here.
    // Categories with no wired actions are omitted from the UI entirely so users don't bind
    // hotkeys that silently do nothing. See functionalHotkeyControls for per-control filtering.
    private val hotkeyCategoryGroups: Map<MenuTag, List<String>> = mapOf(
        MenuTag.HOTKEYS_GENERAL to listOf("General", "Emulation Speed"),
        MenuTag.HOTKEYS_TAS to listOf("Frame Advance"),
        MenuTag.HOTKEYS_VR to listOf("VR"),
        MenuTag.HOTKEYS_SAVE_STATES to listOf("Load State", "Save State"),
        MenuTag.HOTKEYS_ANDROID to listOf("Android"),
    )

    // Untranslated control names (C++ ui_name before GetStringT) that are dispatched by
    // HotkeyDispatcher.cpp. Controls not in this set are hidden within their group so users
    // cannot bind actions that would silently no-op at runtime.
    private val functionalHotkeyControls: Set<String> = setOf(
        "Return to Main Menu",
        "Toggle Pause", "Stop", "Reset", "Take Screenshot",
        "Decrease Emulation Speed", "Increase Emulation Speed", "Disable Emulation Speed Limit",
        "Frame Advance",
        "Toggle OpenXR", "Reset VR Position",
        "Decrease Units Per Meter", "Increase Units Per Meter",
        "Decrease Lean Back Angle", "Increase Lean Back Angle",
        "Toggle Enable Camera Forward", "Decrease Camera Forward", "Increase Camera Forward",
        "Toggle Enable Camera Height", "Decrease Camera Height", "Increase Camera Height",
        "Toggle Virtual Screen",
        "Decrease Screen Distance", "Increase Screen Distance",
        "Decrease Screen Size", "Increase Screen Size",
        "Decrease Screen Curvature", "Increase Screen Curvature",
        "Toggle Don't Clear Screen", "Toggle Force VBI", "Toggle Remove Cinematic Bars",
        "Load State Slot 1", "Load State Slot 2", "Load State Slot 3", "Load State Slot 4",
        "Load State Slot 5", "Load State Slot 6", "Load State Slot 7", "Load State Slot 8",
        "Load State Slot 9", "Load State Slot 10",
        "Save State Slot 1", "Save State Slot 2", "Save State Slot 3", "Save State Slot 4",
        "Save State Slot 5", "Save State Slot 6", "Save State Slot 7", "Save State Slot 8",
        "Save State Slot 9", "Save State Slot 10",
    )

    private fun addHotkeySettings(sl: ArrayList<SettingsItem>) {
        val hotkeys = EmulatedController.getHotkeys()
        addControllerMetaSettings(sl, hotkeys, QuestVrSettings.HOTKEY_PROFILE_NAME)
        if (BuildConfig.IS_QUEST && hotkeys.getDefaultDevice() == OPENXR_CONTROLLER_DEVICE) {
            addOpenXRControllerMapperSetting(
                sl,
                0,
                OpenXRControllerMapperActivity.TARGET_HOTKEYS
            )
        }

        sl.add(HeaderSetting(context, R.string.hotkey_categories, 0))
        sl.add(SubmenuSetting(context, R.string.hotkey_android, MenuTag.HOTKEYS_ANDROID))
        sl.add(SubmenuSetting(context, R.string.hotkey_general, MenuTag.HOTKEYS_GENERAL))
        sl.add(SubmenuSetting(context, R.string.hotkey_tas, MenuTag.HOTKEYS_TAS))
        sl.add(SubmenuSetting(context, R.string.hotkey_vr, MenuTag.HOTKEYS_VR))
        sl.add(SubmenuSetting(context, R.string.hotkey_save_states, MenuTag.HOTKEYS_SAVE_STATES))
    }

    private fun addHotkeyCategorySettings(sl: ArrayList<SettingsItem>, menuTag: MenuTag) {
        val hotkeys = EmulatedController.getHotkeys()
        val wantedGroupNames = hotkeyCategoryGroups[menuTag] ?: return

        val groupCount = hotkeys.getGroupCount()
        for (wantedName in wantedGroupNames) {
            for (i in 0 until groupCount) {
                val group = hotkeys.getGroup(i)
                // Match the untranslated `name` (HotkeyManager passes a single string for both
                // name and ui_name, so this matches the C++ s_groups_info entry under any locale).
                if (!group.getName().equals(wantedName, ignoreCase = true)) continue

                // Filter to only controls wired in HotkeyDispatcher.cpp. Controls not in
                // functionalHotkeyControls would silently no-op if bound, so we hide them.
                val controlCount = group.getControlCount()
                val functional = (0 until controlCount)
                    .map { group.getControl(it) }
                    .filter { it.getName() in functionalHotkeyControls }

                if (functional.isNotEmpty()) {
                    sl.add(HeaderSetting(group.getUiName(), ""))
                    for (control in functional) {
                        sl.add(InputMappingControlSetting(control, hotkeys))
                    }
                }
                break
            }
        }
    }

    /**
     * Adds controller settings that can be set on a per-game basis.
     *
     * @param sl               The list to place controller settings into.
     * @param profileString    The prefix used for the profile setting in game INI files.
     * @param controllerNumber The index of the controller, 0-3.
     */
    private fun addControllerPerGameSettings(
        sl: ArrayList<SettingsItem>,
        controller: EmulatedController,
        controllerNumber: Int
    ) {
        val profiles = ProfileDialogPresenter(menuTag).getProfileNames(false)
        val profileKey = controller.getProfileKey() + "Profile" + (controllerNumber + 1)
        sl.add(
            StringSingleChoiceSetting(
                context,
                AdHocStringSetting(Settings.FILE_GAME_SETTINGS_ONLY, "Controls", profileKey, ""),
                R.string.input_profile,
                0,
                profiles,
                profiles,
                R.string.input_profiles_empty
            )
        )
    }

    /**
     * Adds settings and actions that apply to a controller as a whole.
     * For instance, the device setting and the Clear action.
     *
     * @param sl                  The list to place controller settings into.
     * @param controller          The controller to add settings for.
     * @param questDefaultProfile Stock OpenXR profile that "Default" restores on Quest builds.
     *                            Null keeps Dolphin's built-in defaults.
     */
    private fun addControllerMetaSettings(
        sl: ArrayList<SettingsItem>,
        controller: EmulatedController,
        questDefaultProfile: String? = null
    ) {
        sl.add(
            InputDeviceSetting(
                context,
                R.string.input_device,
                0,
                controller
            )
        )

        sl.add(SwitchSetting(context, object : AbstractBooleanSetting {
            override val isOverridden: Boolean = false

            override val isRuntimeEditable: Boolean = true

            override fun delete(settings: Settings): Boolean {
                fragmentView.isMappingAllDevices = false
                return true
            }

            override val boolean: Boolean
                get() = fragmentView.isMappingAllDevices

            override fun setBoolean(settings: Settings, newValue: Boolean) {
                fragmentView.isMappingAllDevices = newValue
            }
        }, R.string.input_device_all_devices, R.string.input_device_all_devices_description))

        sl.add(
            RunRunnable(
                context,
                R.string.input_reset_to_default,
                R.string.input_reset_to_default_description,
                R.string.input_reset_warning,
                0,
                true
            ) { loadDefaultControllerSettings(controller, questDefaultProfile) })
        sl.add(
            RunRunnable(
                context,
                R.string.input_clear,
                R.string.input_clear_description,
                R.string.input_reset_warning,
                0,
                true
            ) { clearControllerSettings(controller) })
        sl.add(
            RunRunnable(
                context,
                R.string.input_profiles,
                0,
                0,
                0,
                true
            ) { fragmentView.showDialogFragment(ProfileDialog.create(menuTag)) })

        updateOldControllerSettingsWarningVisibility(controller)
    }

    /**
     * Adds mapping settings and other control-specific settings.
     *
     * @param sl              The list to place controller settings into.
     * @param controller      The controller to add settings for.
     * @param groupTypeFilter If this is non-null, only groups whose types match this are considered.
     */
    private fun addControllerMappingSettings(
      sl: ArrayList<SettingsItem>,
      controller: EmulatedController,
      groupTypeFilter: Set<Int>?
    ) {
      addContainerMappingSettings(sl, controller, controller, groupTypeFilter)
    }

    /**
     * Adds mapping settings and other control-specific settings.
     *
     * @param sl              The list to place controller settings into.
     * @param controller      The encompassing controller.
     * @param container       The container of control groups to add settings for.
     * @param groupTypeFilter If this is non-null, only groups whose types match this are considered.
     */
    private fun addContainerMappingSettings(
        sl: ArrayList<SettingsItem>,
        controller: EmulatedController,
        container: ControlGroupContainer,
        groupTypeFilter: Set<Int>?
    ) {
        updateOldControllerSettingsWarningVisibility(controller)

        val groupCount = container.getGroupCount()
        for (i in 0 until groupCount) {
            val group = container.getGroup(i)
            val groupType = group.getGroupType()
            if (groupTypeFilter != null && !groupTypeFilter.contains(groupType)) continue

            sl.add(HeaderSetting(group.getUiName(), ""))

            if (group.getDefaultEnabledValue() != ControlGroup.DEFAULT_ENABLED_ALWAYS) {
                sl.add(
                    SwitchSetting(
                        context,
                        ControlGroupEnabledSetting(group),
                        R.string.enabled,
                        0
                    )
                )
            }

            val controlCount = group.getControlCount()
            for (j in 0 until controlCount) {
                sl.add(InputMappingControlSetting(group.getControl(j), controller))
            }

            if (groupType == ControlGroup.TYPE_ATTACHMENTS) {
                val attachmentSetting = group.getAttachmentSetting()
                sl.add(
                    SingleChoiceSetting(
                        context, InputMappingIntSetting(attachmentSetting),
                        R.string.wiimote_extensions, 0, R.array.wiimoteExtensionsEntries,
                        R.array.wiimoteExtensionsValues,
                        MenuTag.getWiimoteExtensionMenuTag(controllerNumber)
                    )
                )
            }

            val numericSettingCount = group.getNumericSettingCount()
            for (j in 0 until numericSettingCount) {
                addNumericSetting(sl, group.getNumericSetting(j))
            }
        }
    }

    private fun addNumericSetting(sl: ArrayList<SettingsItem>, setting: NumericSetting) {
        when (setting.getType()) {
            NumericSetting.TYPE_DOUBLE -> sl.add(
                FloatSliderSetting(
                    InputMappingDoubleSetting(setting),
                    setting.getUiName(),
                    "",
                    ceil(setting.getDoubleMin()).toFloat(),
                    floor(setting.getDoubleMax()).toFloat(),
                    setting.getUiSuffix(),
                    0.5f,
                    true
                )
            )

            NumericSetting.TYPE_BOOLEAN -> sl.add(
                SwitchSetting(
                    InputMappingBooleanSetting(setting),
                    setting.getUiName(),
                    setting.getUiDescription()
                )
            )
        }
    }

    fun updateOldControllerSettingsWarningVisibility() {
        updateOldControllerSettingsWarningVisibility(menuTag.correspondingEmulatedController)
    }

    private fun updateOldControllerSettingsWarningVisibility(controller: EmulatedController) {
        val defaultDevice = controller.getDefaultDevice()

        hasOldControllerSettings = defaultDevice.startsWith("Android/") &&
                defaultDevice.endsWith("/Touchscreen")

        fragmentView.setOldControllerSettingsWarningVisibility(hasOldControllerSettings)
    }

    private fun loadDefaultControllerSettings(
        controller: EmulatedController,
        questDefaultProfile: String? = null
    ) {
        // loadStockProfile is a no-op off Quest, where Dolphin's own defaults are the right ones.
        val loadedStockProfile = questDefaultProfile != null &&
                QuestVrSettings.loadStockProfile(controller, questDefaultProfile)
        if (!loadedStockProfile) {
            controller.loadDefaultSettings()
        }
        fragmentView.onControllerSettingsChanged()
    }

    private fun clearControllerSettings(controller: EmulatedController) {
        controller.clearSettings()
        fragmentView.onControllerSettingsChanged()
    }

    fun setAllLogTypes(value: Boolean) {
        val settings = fragmentView.settings

        for (logType in NativeLibrary.GetLogTypeNames()) {
            AdHocBooleanSetting(
                Settings.FILE_LOGGER,
                Settings.SECTION_LOGGER_LOGS,
                logType.first,
                false
            ).setBoolean(settings!!, value)
        }

        fragmentView.adapter!!.notifyAllSettingsChanged()
    }

    private fun convertOnThread(f: BooleanSupplier) {
        ThreadUtil.runOnThreadAndShowResult(
            fragmentView.fragmentActivity,
            R.string.wii_converting,
            0,
            { context.resources.getString(if (f.get()) R.string.wii_convert_success else R.string.wii_convert_failure) }
        )
    }

    fun installDriver(uri: Uri) {
        val context = this.context.applicationContext
        CoroutineScope(Dispatchers.IO).launch {
            val stream = context.contentResolver.openInputStream(uri)
            if (stream == null) {
                GpuDriverHelper.uninstallDriver()
                withContext(Dispatchers.Main) {
                    fragmentView.onDriverInstallDone(GpuDriverInstallResult.FileNotFound)
                }
                return@launch
            }

            val result = GpuDriverHelper.installDriver(stream)
            withContext(Dispatchers.Main) {
                with(this@SettingsFragmentPresenter) {
                    this.gpuDriver = GpuDriverHelper.getInstalledDriverMetadata()
                        ?: GpuDriverHelper.getSystemDriverMetadata(context) ?: return@withContext
                    this.libNameSetting.setString(this.settings!!, this.gpuDriver!!.libraryName)
                }
                fragmentView.onDriverInstallDone(result)
            }
        }
    }

    fun useSystemDriver() {
        CoroutineScope(Dispatchers.IO).launch {
            GpuDriverHelper.uninstallDriver()
            withContext(Dispatchers.Main) {
                with(this@SettingsFragmentPresenter) {
                    this.gpuDriver =
                        GpuDriverHelper.getInstalledDriverMetadata()
                            ?: GpuDriverHelper.getSystemDriverMetadata(context.applicationContext)
                    this.libNameSetting.setString(this.settings!!, "")
                }
                fragmentView.onDriverUninstallDone()
            }
        }
    }

    companion object {
        const val ARG_CONTROLLER_TYPE = "controller_type"
        const val ARG_SERIALPORT1_TYPE = "serialport1_type"
        const val ARG_REVISION = "revision"
        private const val OPENXR_CONTROLLER_DEVICE = "OpenXR/0/OpenXR Controller"

        // Value obtained from LogLevel in Common/Logging/Log.h
        private fun getLogVerbosityEntries(): Int {
            // GetMaxLogLevel is effectively a constant, but we can't call it before loading
            // the native library
            return if (NativeLibrary.GetMaxLogLevel() == 5) {
                R.array.logVerbosityEntriesMaxLevelDebug
            } else {
                R.array.logVerbosityEntriesMaxLevelInfo
            }
        }

        // Value obtained from LogLevel in Common/Logging/Log.h
        private fun getLogVerbosityValues(): Int {
            // GetMaxLogLevel is effectively a constant, but we can't call it before loading
            // the native library
            return if (NativeLibrary.GetMaxLogLevel() == 5) {
                R.array.logVerbosityValuesMaxLevelDebug
            } else {
                R.array.logVerbosityValuesMaxLevelInfo
            }
        }
    }
}
