// CustomLcdDisplay 核心类
//
// 负责：
// - 构造/析构（初始化 RLCD 驱动 + LVGL + 创建 UI）
// - LVGL flush 回调（RGB565 → 1-bit 转换）
// - 顶栏中央「小智」状态图标（AI 卡已移除，状态只用图标表达）
// - 备忘录功能（加载/刷新备忘录列表）
// - 基类方法重写（UpdateStatusBar / SetTheme）
//
// 其他功能拆分到独立文件：
//   rlcd_driver.cc        - RLCD 硬件驱动
//   weather_ui.cc          - 天气页布局（时钟卡/合并卡/照片轮播/书目列表）
//   music_ui.cc            - 音乐页布局
//   pomodoro_ui.cc         - 番茄钟布局
//   reader_ui.cc           - 阅读页布局
//   system_ui.cc           - 系统信息页布局
//   data_update_task.cc    - 后台数据更新任务
//   managers/reader_manager.cc - 电子书（扫描/编码/章节/分页/进度）

#include <vector>
#include <string>
#include <cstring>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "lvgl_theme.h"

static const char *TAG = "CustomDisplay";

LV_FONT_DECLARE(font_puhui_16_4)

// ===== LVGL flush 回调 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *self = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    RlcdDriver *rlcd = self->rlcd_;
    uint16_t *buffer = (uint16_t *)color_p;
    for(int y = area->y1; y <= area->y2; y++)
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            rlcd->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    rlcd->RLCD_Display();
    lv_disp_flush_ready(disp);
}

// ===== 构造 / 析构 =====

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    spi_display_config_t spiconfig,
    spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height)
{
    // 1. 初始化 RLCD 硬件驱动
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host);

    // 2. 初始化 LVGL
    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    int transfer = width * height;
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 3. 初始化 RLCD 屏幕
    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    rlcd_->RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    // 4. 创建五页 UI
    ESP_LOGI(TAG, "创建天气页 + 音乐页 + 番茄钟页 + 阅读页 + 系统信息页 UI");
    SetupWeatherUI();
    SetupMusicUI();
    SetupPomodoroUI();
    SetupReaderUI();
    SetupSystemUI();
    // 告诉显示框架：当前自定义 UI 已经初始化完成
    // 否则基类的 SetStatus/ShowNotification 会一直误判为“UI 未准备好”
    setup_ui_called_ = true;
    ApplyDisplayMode();

    // 5. 启动时从 NVS 加载上次保存的备忘录
    LoadMemoFromNvs();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
    delete rlcd_;
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

// 内部版本：不获取锁（调用者必须已持有 DisplayLock）
void CustomLcdDisplay::RefreshMemoDisplayInternal() {
    // 从 NVS 读取 JSON 数组
    Settings settings("memo", false);
    std::string json_str = settings.GetString("items", "");

    memo_lines_.clear();
    if (!json_str.empty()) {
        cJSON *arr = cJSON_Parse(json_str.c_str());
        if (arr && cJSON_IsArray(arr)) {
            int count = cJSON_GetArraySize(arr);
            for (int i = 0; i < count; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *t = cJSON_GetObjectItem(item, "t");
                cJSON *c = cJSON_GetObjectItem(item, "c");

                // 格式：[时间] 内容  或  · 内容（无时间时）
                std::string line;
                if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
                    line = t->valuestring;
                    line += " ";
                } else {
                    line = "· ";
                }
                if (c && cJSON_IsString(c)) {
                    line += c->valuestring;
                }
                memo_lines_.push_back(line);
            }
            ESP_LOGI(TAG, "备忘列表已刷新，共 %d 条", count);
        }
        if (arr) cJSON_Delete(arr);
    }

    memo_view_idx_ = 0;
    RenderMemoWindow();
}

// 外部版本：自动获取锁（供 MCP 工具等外部调用）
void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    RefreshMemoDisplayInternal();
}

// ===== AI 显示方法（AI 对话卡已移除，全部改为空实现）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    // AI 回答不再上屏，由顶栏中央状态图标（SetChatUiState）表达交互状态
    (void)role;
    (void)content;
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    // 表情卡已移除
    (void)emotion;
}

void CustomLcdDisplay::ClearChatMessages() {
    // 无需清理
}

// ===== 顶栏中央「小智」+ 状态图标 =====

void CustomLcdDisplay::CreateTopStatus(lv_obj_t *page) {
    lv_obj_t *name = lv_label_create(page);
    lv_obj_set_style_text_font(name, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, "小智");
    lv_obj_set_pos(name, 174, 6);

    lv_obj_t *icon = lv_obj_create(page);
    lv_obj_set_size(icon, 16, 16);
    lv_obj_set_pos(icon, 210, 8);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(icon, (void*)(intptr_t)chat_ui_state_);
    lv_obj_add_event_cb(icon, StatusIconDrawEvent, LV_EVENT_DRAW_MAIN, nullptr);
    status_icons_.push_back(icon);
}

// 状态图标绘制（三态：0=待命空心圆 1=聆听脉冲环 2=说话实心+声波）
void CustomLcdDisplay::StatusIconDrawEvent(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    intptr_t st = (intptr_t)lv_obj_get_user_data(obj);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t cx = (coords.x1 + coords.x2) / 2;
    int32_t cy = (coords.y1 + coords.y2) / 2;
    uint32_t tick = lv_tick_get();

    if (st == 2) {
        // 说话：实心圆 + 两侧声波交替
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = lv_color_white();
        rd.bg_opa = LV_OPA_COVER;
        rd.radius = LV_RADIUS_CIRCLE;
        lv_area_t dot = { cx - 4, cy - 4, cx + 3, cy + 3 };
        lv_draw_rect(layer, &rd, &dot);

        if ((tick / 300) % 2 == 0) {
            lv_draw_arc_dsc_t arc;
            lv_draw_arc_dsc_init(&arc);
            arc.color = lv_color_white();
            arc.width = 2;
            arc.center.x = cx;
            arc.center.y = cy;
            arc.radius = 8;
            arc.start_angle = -40;
            arc.end_angle = 40;
            lv_draw_arc(layer, &arc);
            arc.start_angle = 140;
            arc.end_angle = 220;
            lv_draw_arc(layer, &arc);
        }
    } else if (st == 1) {
        // 聆听：内环常亮 + 外环脉冲
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = 2;
        arc.center.x = cx;
        arc.center.y = cy;
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.radius = 4;
        lv_draw_arc(layer, &arc);

        arc.radius = 7;
        uint32_t phase = (tick / 250) % 4;
        arc.opa = (phase == 0) ? LV_OPA_COVER : (phase == 1) ? LV_OPA_50 : LV_OPA_20;
        lv_draw_arc(layer, &arc);
    } else {
        // 待命：空心圆
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = 2;
        arc.center.x = cx;
        arc.center.y = cy;
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.radius = 6;
        lv_draw_arc(layer, &arc);
    }
}

void CustomLcdDisplay::SetChatUiState(int st) {
    // 调用者需已持有 DisplayLock
    if (st == chat_ui_state_) return;
    chat_ui_state_ = st;
    for (auto *icon : status_icons_) {
        if (!icon) continue;
        lv_obj_set_user_data(icon, (void*)(intptr_t)st);
        lv_obj_invalidate(icon);
    }
    // 回到待命后停止动画定时器
    if (status_anim_timer_) {
        if (st == 0) lv_timer_pause(status_anim_timer_);
        else lv_timer_resume(status_anim_timer_);
    }
}

void CustomLcdDisplay::StatusAnimTimerCb(lv_timer_t *t) {
    auto *self = (CustomLcdDisplay *)lv_timer_get_user_data(t);
    if (self->chat_ui_state_ == 0) return;
    for (auto *icon : self->status_icons_) {
        if (icon) lv_obj_invalidate(icon);
    }
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
    (void)update_all;
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    current_theme_ = theme;
    ESP_LOGI(TAG, "RLCD 单色屏，跳过主题切换");
}

void CustomLcdDisplay::ApplyDisplayMode() {
    // 先隐藏所有页面
    if (weather_page_) lv_obj_add_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
    if (music_page_) lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
    if (pomodoro_page_) lv_obj_add_flag(pomodoro_page_, LV_OBJ_FLAG_HIDDEN);
    if (reader_page_) lv_obj_add_flag(reader_page_, LV_OBJ_FLAG_HIDDEN);
    if (system_page_) lv_obj_add_flag(system_page_, LV_OBJ_FLAG_HIDDEN);

    // 显示当前页面
    switch (display_mode_) {
        case MODE_WEATHER:
            if (weather_page_) lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_MUSIC:
            if (music_page_) lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_POMODORO:
            if (pomodoro_page_) lv_obj_remove_flag(pomodoro_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_READER:
            if (reader_page_) lv_obj_remove_flag(reader_page_, LV_OBJ_FLAG_HIDDEN);
            // 首次进入阅读页时懒加载书籍（扫描/转码/分页，一次性开销）
            ReaderEnsureLoaded();
            break;
        case MODE_SYSTEM_INFO:
            if (system_page_) lv_obj_remove_flag(system_page_, LV_OBJ_FLAG_HIDDEN);
            UpdateSystemInfo();
            break;
    }
}

void CustomLcdDisplay::CycleDisplayMode() {
    DisplayLockGuard lock(this);
    // 五页循环：天气 → 音乐 → 番茄钟 → 阅读 → 系统信息 → 天气
    switch (display_mode_) {
        case MODE_WEATHER:    display_mode_ = MODE_MUSIC; break;
        case MODE_MUSIC:      display_mode_ = MODE_POMODORO; break;
        case MODE_POMODORO:   display_mode_ = MODE_READER; break;
        case MODE_READER:     display_mode_ = MODE_SYSTEM_INFO; break;
        case MODE_SYSTEM_INFO:display_mode_ = MODE_WEATHER; break;
    }
    ApplyDisplayMode();
    const char* name = "未知";
    switch (display_mode_) {
        case MODE_WEATHER:  name = "天气页"; break;
        case MODE_MUSIC:    name = "音乐页"; break;
        case MODE_POMODORO: name = "番茄钟"; break;
        case MODE_READER:   name = "阅读页"; break;
        case MODE_SYSTEM_INFO: name = "系统信息"; break;
    }
    ESP_LOGI(TAG, "页面切换: %s", name);
}

void CustomLcdDisplay::SetMusicInfo(const char* title, const char* artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ == nullptr || music_artist_label_ == nullptr) {
        return;
    }
    lv_label_set_text(music_title_label_, (title && strlen(title) > 0) ? title : "未知歌曲");
    lv_label_set_text(music_artist_label_, (artist && strlen(artist) > 0) ? artist : "未知歌手");
}

void CustomLcdDisplay::SetMusicLyric(const char* lyric) {
    DisplayLockGuard lock(this);
    if (music_lyric_label_ == nullptr) {
        return;
    }

    // 歌词格式："上一句\n当前句\n下一句"（由 application.cc 拼接）
    // 如果没有 \n 分隔符，说明是单行文本（如错误提示），直接显示在当前行
    std::string text(lyric ? lyric : "");
    std::string prev_line, curr_line, next_line;

    size_t first_nl = text.find('\n');
    if (first_nl != std::string::npos) {
        prev_line = text.substr(0, first_nl);
        size_t second_nl = text.find('\n', first_nl + 1);
        if (second_nl != std::string::npos) {
            curr_line = text.substr(first_nl + 1, second_nl - first_nl - 1);
            next_line = text.substr(second_nl + 1);
        } else {
            curr_line = text.substr(first_nl + 1);
        }
    } else {
        // 单行文本（错误提示等），只显示在当前行
        curr_line = text;
    }

    // 更新三个 label
    if (music_lyric_prev_label_) {
        lv_label_set_text(music_lyric_prev_label_, prev_line.c_str());
    }
    lv_label_set_text(music_lyric_label_, curr_line.c_str());
    if (music_lyric_next_label_) {
        lv_label_set_text(music_lyric_next_label_, next_line.c_str());
    }
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_progress_bar_ == nullptr || music_progress_label_ == nullptr) {
        return;
    }

    if (total_ms > 0) {
        // 有总时长（来自歌词）：正常显示进度条和 "当前 / 总时长"
        if (current_ms > total_ms) {
            current_ms = total_ms;
        }
        lv_bar_set_range(music_progress_bar_, 0, static_cast<int32_t>(total_ms));
        lv_bar_set_value(music_progress_bar_, static_cast<int32_t>(current_ms), LV_ANIM_OFF);

        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu / %02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60),
                 static_cast<unsigned long>(total_ms / 60000),
                 static_cast<unsigned long>((total_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    } else {
        // 无总时长（没有歌词）：进度条不动，只显示已播放时间
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    }
}

void CustomLcdDisplay::SwitchToMusicPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_MUSIC) {
        display_mode_ = MODE_MUSIC;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到音乐页");
    }
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_WEATHER) {
        display_mode_ = MODE_WEATHER;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到天气页");
    }
}

// ===== 番茄钟页面方法 =====

void CustomLcdDisplay::SwitchToPomodoroPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_POMODORO) {
        display_mode_ = MODE_POMODORO;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到番茄钟页");
    }
}

void CustomLcdDisplay::UpdatePomodoroDisplay(const char* state_text, const char* countdown_text,
                                              int progress_permille, const char* info_text) {
    DisplayLockGuard lock(this);
    if (pomo_state_label_ && state_text) {
        lv_label_set_text(pomo_state_label_, state_text);
    }
    if (pomo_countdown_label_ && countdown_text) {
        lv_label_set_text(pomo_countdown_label_, countdown_text);
    }
    if (pomo_progress_bar_) {
        lv_bar_set_value(pomo_progress_bar_, progress_permille, LV_ANIM_OFF);
    }
    if (pomo_info_label_ && info_text) {
        lv_label_set_text(pomo_info_label_, info_text);
    }
}

// ===== 音量临时胶囊 =====

void CustomLcdDisplay::ShowMusicVolume(int percent) {
    DisplayLockGuard lock(this);
    if (!music_volume_chip_ || !music_volume_label_) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char buf[24];
    snprintf(buf, sizeof(buf), "音量 %d%%", percent);
    lv_label_set_text(music_volume_label_, buf);
    lv_obj_remove_flag(music_volume_chip_, LV_OBJ_FLAG_HIDDEN);
    volume_shown_until_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS + 2000;
    if (volume_hide_timer_) lv_timer_resume(volume_hide_timer_);
    ESP_LOGI(TAG, "音量 %d%%", percent);
}

void CustomLcdDisplay::VolumeHideTimerCb(lv_timer_t *t) {
    auto *self = (CustomLcdDisplay *)lv_timer_get_user_data(t);
    if (self->volume_shown_until_ms_ == 0) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now >= self->volume_shown_until_ms_) {
        if (self->music_volume_chip_ && !lv_obj_has_flag(self->music_volume_chip_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(self->music_volume_chip_, LV_OBJ_FLAG_HIDDEN);
        }
        self->volume_shown_until_ms_ = 0;
        lv_timer_pause(t);
    }
}

// ===== 系统信息页 =====

void CustomLcdDisplay::SwitchToSystemInfoPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_SYSTEM_INFO) {
        display_mode_ = MODE_SYSTEM_INFO;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到系统信息页");
    }
}

// ===== 省电模式 =====

void CustomLcdDisplay::NotifyUserActivity() {
    last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (power_saving_) {
        power_saving_ = false;
        ESP_LOGI(TAG, "用户活动检测到，退出省电模式");
    }
}
