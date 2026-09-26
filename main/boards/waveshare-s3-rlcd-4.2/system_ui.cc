// 系统信息页布局 UI
//
// 负责创建系统信息页的所有 LVGL 控件：
// - 顶栏（时间 + 温湿度 + 中央小智状态图标 + 状态胶囊）
// - 白底信息卡（CPU / 运行 / SRAM / PSRAM / 电池 / WiFi 六行）
//
// 数据读取由 UpdateSystemInfo() 完成（进入本页时 + 数据刷新时调用）。

#include "custom_lcd_display.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <soc/rtc.h>
#include "application.h"
#include "board.h"

// 字体
LV_FONT_DECLARE(alibaba_puhui_16);   // 纯数字/ASCII
LV_FONT_DECLARE(alibaba_puhui_24);   // 时钟数字
LV_FONT_DECLARE(font_puhui_16_4);    // 16px 小智完整字库（中文）
LV_FONT_DECLARE(font_puhui_14_1);    // 14px 小字

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

static const char *TAG = "SystemUI";

static const int SCR_W = 400;
static const int SCR_H = 300;
static const int PAD = 12;

void CustomLcdDisplay::SetupSystemUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_num  = &alibaba_puhui_16;
    const lv_font_t *font_time = &alibaba_puhui_24;
    const lv_font_t *font_cn   = &font_puhui_16_4;
    const lv_font_t *font_sm   = &font_puhui_14_1;

    // ===== 系统信息页容器（全屏黑底，初始隐藏）=====
    system_page_ = lv_obj_create(root);
    lv_obj_set_size(system_page_, SCR_W, SCR_H);
    lv_obj_set_pos(system_page_, 0, 0);
    lv_obj_set_style_bg_color(system_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(system_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(system_page_, 0, 0);
    lv_obj_set_style_pad_all(system_page_, 0, 0);
    lv_obj_set_style_radius(system_page_, 0, 0);
    lv_obj_remove_flag(system_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(system_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = system_page_;

    // ============================================================
    // 顶部信息（时间 + 温湿度 + 中央小智 + 状态胶囊）
    // ============================================================

    sys_time_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(sys_time_label_, font_time, 0);
    lv_obj_set_style_text_color(sys_time_label_, lv_color_white(), 0);
    lv_obj_align(sys_time_label_, LV_ALIGN_TOP_LEFT, 10, 5);
    lv_label_set_text(sys_time_label_, "00:00");

    sys_sensor_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(sys_sensor_label_, font_sm, 0);
    lv_obj_set_style_text_color(sys_sensor_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(sys_sensor_label_, LV_OPA_60, 0);
    lv_obj_align(sys_sensor_label_, LV_ALIGN_TOP_LEFT, 80, 11);
    lv_label_set_text(sys_sensor_label_, "--.-°C --.-%");

    CreateTopStatus(page);

    lv_obj_t *status_bar = lv_obj_create(page);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_set_style_pad_column(status_bar, 5, 0);
    lv_obj_set_style_pad_row(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sys_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(sys_wifi_icon_img_, &ui_img_wifi_off);
    sys_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(sys_battery_icon_img_, &ui_img_battery_full);
    sys_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(sys_battery_pct_label_, font_num, 0);
    lv_obj_set_style_text_color(sys_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(sys_battery_pct_label_, "---%");

    // ============================================================
    // 白底信息卡（8,36,384,256）
    // ============================================================

    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_pos(card, 8, 36);
    lv_obj_set_size(card, 384, 256);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 15, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, font_cn, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, 360);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(title, "系统信息");

    // 标题下分隔线
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_set_size(sep, 348, 2);
    lv_obj_set_pos(sep, 16, 30);
    lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    // 六行：名称（左） + 值（右对齐）
    static const char *row_names[] = {
        "CPU", "运行", "SRAM", "PSRAM", "电池", "WiFi"
    };
    const int row_y0 = 44;
    const int row_step = 34;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *name = lv_label_create(card);
        lv_obj_set_style_text_font(name, font_cn, 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_set_pos(name, 20, row_y0 + row_step * i);
        lv_label_set_text(name, row_names[i]);

        lv_obj_t *value = lv_label_create(card);
        lv_obj_set_style_text_font(value, font_sm, 0);
        lv_obj_set_style_text_color(value, lv_color_black(), 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(value, 140, row_y0 + row_step * i + 2);
        lv_obj_set_width(value, 224);
        lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_label_set_text(value, "--");
        sys_value_labels_[i] = value;
    }

    ESP_LOGI(TAG, "系统信息页 UI 创建完成");
}

// ===== 数据读取与刷新（自动加锁）=====

void CustomLcdDisplay::UpdateSystemInfo() {
    DisplayLockGuard lock(this);
    if (!system_page_) return;

    char buf[64];

    // CPU 主频
    rtc_cpu_freq_config_t cpu_conf;
    rtc_clk_cpu_freq_get_config(&cpu_conf);
    snprintf(buf, sizeof(buf), "%luMHz", static_cast<unsigned long>(cpu_conf.freq_mhz));
    lv_label_set_text(sys_value_labels_[0], buf);

    // 运行时间
    uint64_t uptime_sec = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%luh %lumin",
             static_cast<unsigned long>(uptime_sec / 3600),
             static_cast<unsigned long>((uptime_sec % 3600) / 60));
    lv_label_set_text(sys_value_labels_[1], buf);

    // SRAM
    size_t free_heap = esp_get_free_heap_size();
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    int heap_percent = total_heap > 0
        ? static_cast<int>(((total_heap - free_heap) * 100.0f) / total_heap) : 0;
    snprintf(buf, sizeof(buf), "%dKB / %dKB (%d%%)",
             static_cast<int>((total_heap - free_heap) / 1024),
             static_cast<int>(total_heap / 1024), heap_percent);
    lv_label_set_text(sys_value_labels_[2], buf);

    // PSRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    int psram_percent = total_psram > 0
        ? static_cast<int>(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
    snprintf(buf, sizeof(buf), "%dMB / %dMB (%d%%)",
             static_cast<int>((total_psram - free_psram) / 1024 / 1024),
             static_cast<int>(total_psram / 1024 / 1024), psram_percent);
    lv_label_set_text(sys_value_labels_[3], buf);

    // 电池
    int battery_level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        snprintf(buf, sizeof(buf), "%d%% %s", battery_level, charging ? "充电中" : "放电中");
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(sys_value_labels_[4], buf);

    // WiFi
    auto ds = Application::GetInstance().GetDeviceState();
    const char *wifi_status = "未连接";
    if (ds == kDeviceStateWifiConfiguring) wifi_status = "配网中";
    else if (ds != kDeviceStateStarting) wifi_status = "已连接";
    lv_label_set_text(sys_value_labels_[5], wifi_status);
}
